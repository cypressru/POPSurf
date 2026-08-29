#!/usr/bin/env python3
"""gmbake - bake a General MIDI sample bank for the Dreamcast AICA.

Renders one note per GM melodic program and one hit per GM percussion note out
of a SoundFont with fluidsynth, then encodes each to 4-bit Yamaha ADPCM - the
format the AICA decodes in hardware. The whole bank lives in the 2MB of SPU RAM
that is separate from main RAM, so playing GM costs the SH-4 nothing but note
on/off.

    tools/gmbake.py TimGM6mb.sf2 dist/gmbank.psb

Everything lands in one .psb container so startup is a single sequential read
rather than 175 GD-ROM seeks. Requires fluidsynth on PATH and numpy. See
tools/README.md for the on-disk layout.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

RATE = 22050
ROOT_NOTE = 60          # middle C; every melodic sample is recorded here
VELOCITY = 100

# AICA plays two samples per ADPCM byte and spu_memload_sq moves whole 32-byte
# store-queue blocks, so every sample length is forced to a multiple of 64
# frames. That makes the byte length an exact multiple of 32 with no padding.
FRAME_ALIGN = 64

KOS_MAX_FRAMES = 65534  # snd_sfx_load_raw_buf warns past this

MELODIC_FRAMES = 13248          # 0.601 s, 207 * 64  (--melodic-frames)
DRUM_MIN_FRAMES = 64 * 52       # 0.151 s
DRUM_MAX_FRAMES = 64 * 242      # 0.702 s

XFADE_MS = 25.0         # loop wrap crossfade
LOOP_SEARCH = 512       # frames of slack for phase-aligning the loop window
FADEOUT_MS = 15.0       # one-shot tail fade, kills the cutoff click
DRUM_TAIL_DB = -45.0    # trim a drum once it falls this far below its peak

RENDER_HOLD_S = 1.6     # how long the note is held in the generated MIDI
RENDER_TOTAL_S = 2.0
DRUM_HOLD_S = 0.1
DRUM_TOTAL_S = 1.2

DRUM_NOTES = range(35, 82)

# An instrument is looped when it actually holds a tone while the key is down.
# This has to be measured rather than assumed: a table of "GM programs that
# sustain" only describes General MIDI, and a bank baked from an arbitrary
# SoundFont may put anything at any program number. Deciding from the rendered
# envelope works for any font.
SUSTAIN_RATIO = 0.35    # level at end of hold vs peak, above which it sustains
MIN_LOOP_FRAMES = 4096  # a loop shorter than this buzzes rather than sustains


def is_sustaining(x):
    """True when the note is still sounding near the end of the held period.

    A struck or plucked sound has decayed well below its peak by then; a bowed,
    blown or drawn one has not."""
    env, win = envelope(x)
    if env.size == 0:
        return False
    peak = float(env.max())
    if peak <= 0.0:
        return False
    hold_end = int(RATE * (RENDER_HOLD_S - 0.15)) // win
    if hold_end <= 0 or hold_end >= env.size:
        hold_end = env.size - 1
    tail = float(env[max(0, hold_end - 3):hold_end + 1].mean())
    return tail / peak >= SUSTAIN_RATIO


KIND_MELODIC = 0
KIND_DRUM = 1


# ---------------------------------------------------------------------------
# Standard MIDI File generation
# ---------------------------------------------------------------------------

def _vlq(value):
    """MIDI variable-length quantity."""
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    return bytes(reversed(out))


def build_smf(channel, program, note, hold_s, total_s, division=480):
    """One-note SMF format 0. Tempo is fixed at 500000us/quarter so a tick is
    exactly 1/960 s and the hold times below convert cleanly."""
    ticks_per_s = division * 2.0
    track = b""
    track += _vlq(0) + b"\xff\x51\x03" + struct.pack(">I", 500000)[1:]
    if program is not None:
        track += _vlq(0) + bytes([0xC0 | channel, program])
    track += _vlq(0) + bytes([0x90 | channel, note, VELOCITY])
    track += _vlq(int(hold_s * ticks_per_s)) + bytes([0x80 | channel, note, 64])
    track += _vlq(int((total_s - hold_s) * ticks_per_s)) + b"\xff\x2f\x00"
    return (b"MThd" + struct.pack(">IHHH", 6, 0, 1, division)
            + b"MTrk" + struct.pack(">I", len(track)) + track)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def render_one(sf2, mid_path, wav_path, gain):
    cmd = [
        "fluidsynth",
        "-F", wav_path,
        "-T", "wav",
        "-r", str(RATE),
        "-g", str(gain),
        "-R", "0",          # no reverb: it smears the tail and inflates length
        "-C", "0",          # no chorus: same reason, and it detunes the loop
        "-q", "-i",
        sf2, mid_path,
    ]
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    return proc.returncode == 0 and os.path.exists(wav_path)


def read_wav_mono(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("%s: expected 16-bit WAV" % path)
        channels = w.getnchannels()
        raw = w.readframes(w.getnframes())
    data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    return data


# ---------------------------------------------------------------------------
# Sample conditioning
# ---------------------------------------------------------------------------

def dc_block(x):
    """Subtract a ~50ms moving average, i.e. a high-pass around 20Hz. A DC
    offset thumps on every loop wrap and clicks at the start of a one-shot, and
    SoundFont samples carry plenty. Done as a running sum rather than a
    per-sample recursion because the recursion is far too slow in Python."""
    win = int(RATE / 20.0) | 1
    if x.shape[0] < win * 2:
        return x - x.mean()
    pad = win // 2
    padded = np.concatenate((np.full(pad, x[0]), x, np.full(pad, x[-1])))
    csum = np.concatenate(([0.0], np.cumsum(padded, dtype=np.float64)))
    avg = (csum[win:win + x.shape[0]] - csum[:x.shape[0]]) / win
    return (x - avg).astype(np.float32)


def envelope(x, win_ms=10.0):
    """Windowed RMS, one value per window, used for onset and tail detection."""
    win = max(1, int(RATE * win_ms / 1000.0))
    n = (x.shape[0] // win) * win
    if n == 0:
        return np.zeros(1), win
    blocks = x[:n].reshape(-1, win)
    return np.sqrt((blocks.astype(np.float64) ** 2).mean(axis=1)), win


def find_onset(x, thresh=0.02):
    peak = np.abs(x).max()
    if peak <= 0.0:
        return 0
    hits = np.nonzero(np.abs(x) > peak * thresh)[0]
    if hits.size == 0:
        return 0
    # back off 2ms so the very start of the transient survives the cut
    return max(0, int(hits[0]) - int(RATE * 0.002))


def global_gain(peaks, target):
    """One multiplier for the whole bank, sized so the single loudest sample
    lands on `target`.

    Deliberately not per-sample normalisation. GM mixes depend on instruments
    keeping their relative levels - normalising each sample would make a
    triangle as loud as a piano - and the container has no per-entry gain field
    to undo it with. Scaling everything by one number moves the bank up without
    touching the balance the SoundFont set.
    """
    loudest = max(peaks) if peaks else 0.0
    if loudest <= 0.0:
        return 1.0
    return target / loudest


def fade_out(x, ms=FADEOUT_MS):
    n = min(x.shape[0], int(RATE * ms / 1000.0))
    if n > 1:
        x[-n:] *= np.linspace(1.0, 0.0, n, dtype=np.float32)
    return x


def make_loop(x, length, xfade):
    """Lay out `length` frames as [attack][sustain loop] and return the frame
    the loop restarts from.

    The attack is the half of a note that carries its identity - the bow bite,
    the reed chiff, the hammer - and most of its high frequency energy. An
    earlier version of this started the buffer at the envelope peak so the
    whole buffer could loop, which threw the attack away: every sustained
    instrument began at full level with no transient, which measured as four
    times too little dynamic range and a much duller spectrum than the source.

    The AICA has separate loop-start and loop-end registers, so the attack can
    simply sit in front of the loop and be played once."""
    start = find_onset(x)
    x = x[start:]
    if x.shape[0] < length + xfade + LOOP_SEARCH:
        x = np.concatenate(
            (x, np.zeros(length + xfade + LOOP_SEARCH - x.shape[0],
                         dtype=np.float32)))

    env, win = envelope(x)
    peak = int(env.argmax()) * win

    # The loop starts after the attack has settled, but must leave a long
    # enough region to sound like a tone rather than a buzz.
    ls = max(xfade, min(peak, length - MIN_LOOP_FRAMES))

    # Wrapping from `length` back to `ls` must not jump in phase, or the loop
    # buzzes at its own rate. Slide the start point to where the waveform best
    # matches what follows the end of the buffer.
    lo = max(xfade, ls - LOOP_SEARCH // 2)
    hi = min(length - MIN_LOOP_FRAMES, ls + LOOP_SEARCH // 2)
    if hi > lo:
        cand = sliding_window_view(x[lo:hi + xfade], xfade)
        target = x[length:length + xfade]
        ls = lo + int(((cand - target) ** 2).sum(axis=1).argmin())

    out = x[:length].copy()

    # Crossfade the end of the loop into the material just before the loop
    # start, so the frame before the wrap joins the frame after it.
    if ls >= xfade and xfade > 1:
        t = np.linspace(0.0, 1.0, xfade, dtype=np.float32)
        out[length - xfade:length] = (out[length - xfade:length] * np.sqrt(1.0 - t)
                                      + x[ls - xfade:ls] * np.sqrt(t))
    return out, ls


def align_frames(n):
    return max(FRAME_ALIGN, ((n + FRAME_ALIGN - 1) // FRAME_ALIGN) * FRAME_ALIGN)


def prepare_melodic(x, looped):
    x = dc_block(x)
    loop_start = 0
    if looped:
        out, loop_start = make_loop(x, MELODIC_FRAMES,
                                    int(RATE * XFADE_MS / 1000.0))
    else:
        start = find_onset(x)
        out = x[start:start + MELODIC_FRAMES]
        if out.shape[0] < MELODIC_FRAMES:
            out = np.concatenate(
                (out, np.zeros(MELODIC_FRAMES - out.shape[0], dtype=np.float32)))
        out = fade_out(out.copy())
    return out, loop_start


def prepare_drum(x):
    x = dc_block(x)
    start = find_onset(x)
    x = x[start:]
    env, win = envelope(x, 5.0)
    if env.size and env.max() > 0.0:
        floor = env.max() * (10.0 ** (DRUM_TAIL_DB / 20.0))
        live = np.nonzero(env > floor)[0]
        end = (int(live[-1]) + 1) * win if live.size else DRUM_MIN_FRAMES
    else:
        end = DRUM_MIN_FRAMES
    end += int(RATE * FADEOUT_MS / 1000.0)
    end = align_frames(min(max(end, DRUM_MIN_FRAMES), DRUM_MAX_FRAMES))
    out = x[:end]
    if out.shape[0] < end:
        out = np.concatenate((out, np.zeros(end - out.shape[0], dtype=np.float32)))
    out = fade_out(out.copy())
    return out


# ---------------------------------------------------------------------------
# Yamaha / AICA 4-bit ADPCM
#
# Byte-for-byte the algorithm in KallistiOS utils/wav2adpcm (superctr's public
# domain YMZ codec). Even samples go in the low nibble, odd in the high nibble,
# which is the order the AICA decodes them in.
# ---------------------------------------------------------------------------

_STEP_TABLE = (230, 230, 230, 230, 307, 409, 512, 614)


def _clamp(v, lo, hi):
    return hi if v > hi else (lo if v < lo else v)


def pcm_to_adpcm(pcm16):
    """pcm16: int16 numpy array. Returns bytes, len == len(pcm16)//2."""
    out = bytearray(len(pcm16) // 2)
    step_size = 127
    history = 0
    buf_sample = 0
    nibble = 0
    o = 0
    for sample in pcm16.tolist():
        # drop three bits of the residual before quantising; the encoder in KOS
        # does this to keep the step search from chasing dither
        step = (sample & -8) - history
        code = (abs(step) << 16) // (step_size << 14)
        code = _clamp(code, 0, 7)
        if step < 0:
            code |= 8
        if not nibble:
            buf_sample = code & 0x0F
        else:
            out[o] = buf_sample | (code << 4)
            o += 1
        nibble ^= 1
        # advance the predictor exactly as the decoder will
        delta = code & 7
        diff = ((1 + (delta << 1)) * step_size) >> 3
        diff = _clamp(diff, 0, 32767)
        history = history - diff if (code & 8) else history + diff
        history = _clamp(history, -32768, 32767)
        step_size = _clamp((_STEP_TABLE[delta] * step_size) >> 8, 127, 24576)
    return bytes(out)


def adpcm_to_pcm(data, highpass=False):
    """Reference decode. `highpass` matches KOS adpcm2pcm, which leaks the
    predictor by 254/256 per sample; the encoder does not, so the round-trip
    figure with it on is a KOS quirk rather than a coding error."""
    out = np.empty(len(data) * 2, dtype=np.int16)
    step_size = 127
    history = 0
    i = 0
    for byte in data:
        for code in (byte & 0x0F, byte >> 4):
            if highpass:
                history = int(history * 254 / 256)
            delta = code & 7
            diff = ((1 + (delta << 1)) * step_size) >> 3
            diff = _clamp(diff, 0, 32767)
            history = history - diff if (code & 8) else history + diff
            history = _clamp(history, -32768, 32767)
            step_size = _clamp((_STEP_TABLE[delta] * step_size) >> 8, 127, 24576)
            out[i] = history
            i += 1
    return out


def to_pcm16(x):
    return np.clip(np.rint(x * 32767.0), -32768, 32767).astype(np.int16)


def to_pcm8s(x):
    """AICA 8-bit PCM is signed two's complement, not offset binary. Writing
    unsigned here plays as a DC-offset, half-inverted mess."""
    return np.clip(np.rint(x * 127.0), -128, 127).astype(np.int8)


# ---------------------------------------------------------------------------
# .psb container
# ---------------------------------------------------------------------------

# v4 adds a per-sample loop start, so the attack can precede the loop.
# v3 added the sample format to the header; v2 banks were always ADPCM.
BANK_VERSION = 4
HEADER_SIZE = 20

FMT_ADPCM = 0
FMT_PCM8 = 1
ENTRY_SIZE = 24


def _u32(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF))


def write_parts(path, part_bytes):
    """Also write the bank as .p00, .p01 ... alongside the whole file.

    Both are published: the parts are what the browser asks for first, and the
    single file stays as the fallback for anywhere that only has one URL."""
    if part_bytes <= 0:
        return 0

    data = open(path, "rb").read()
    n = 0
    for off in range(0, len(data), part_bytes):
        with open("%s.p%02d" % (path, n), "wb") as f:
            f.write(data[off:off + part_bytes])
        n += 1

    # A stale longer set from a previous, larger bake would be read as extra
    # data appended to this one.
    i = n
    while os.path.exists("%s.p%02d" % (path, i)):
        os.remove("%s.p%02d" % (path, i))
        i += 1
    return n


def write_bank(path, entries, fmt):
    """One sequential blob: header, entry table, then every sample's ADPCM
    packed end to end. Separate files would cost a GD-ROM seek each at startup.

    Every field is written as explicit shifted bytes so the bake does not
    depend on the endianness of the machine that ran it.
    """
    data_off = HEADER_SIZE + len(entries) * ENTRY_SIZE
    blob = bytearray()
    blob += bytes((ord("P"), ord("S"), ord("G"), ord("M")))
    blob += _u32(BANK_VERSION)
    blob += _u32(len(entries))
    blob += _u32(data_off)
    blob += _u32(fmt)
    for e in entries:
        blob += bytes((e["kind"] & 0xFF, e["index"] & 0xFF,
                       e["root"] & 0xFF, e["loop"] & 0xFF))
        blob += _u32(e["rate"])
        blob += _u32(e["frames"])
        blob += _u32(e["offset"])
        blob += _u32(len(e["blob"]))
        blob += _u32(e["loop_start"])
    assert len(blob) == data_off
    for e in entries:
        blob += e["blob"]
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob), data_off


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main():
    global MELODIC_FRAMES
    ap = argparse.ArgumentParser(description="Bake a GM sample bank for AICA")
    ap.add_argument("soundfont")
    ap.add_argument("output", nargs="?", default="dist/gmbank.psb")
    ap.add_argument("--part-bytes", type=int, default=720000,
                    help="split the bank into .p00, .p01 ... files of at most "
                         "this size, which is how the browser fetches it. A "
                         "single multi-megabyte response is minutes on a modem "
                         "with no way to show progress, and does not survive "
                         "every link. 0 writes one file.")
    ap.add_argument("--melodic-frames", type=int, default=MELODIC_FRAMES,
                    help="frames per melodic sample; must be a multiple of 64. "
                         "Shorter samples make a smaller bank at the cost of "
                         "one-shot decays, which is a useful trade when the "
                         "bank has to cross a slow or fragile link.")
    ap.add_argument("--format", choices=("adpcm", "pcm8"), default="pcm8",
                    help="pcm8 = 8-bit signed PCM (default). adpcm is half the "
                         "size but the AICA cannot pitch-shift it more than "
                         "about an octave without audible breakup, which a "
                         "single-root-note GM bank does constantly.")
    ap.add_argument("--gain", type=float, default=0.8,
                    help="fluidsynth master gain")
    ap.add_argument("--target-peak", type=float, default=0.9,
                    help="the loudest sample in the bank is scaled to this; "
                         "one global multiplier, so relative instrument "
                         "balance is preserved")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--keep-renders", metavar="DIR",
                    help="keep the intermediate fluidsynth WAVs here")
    args = ap.parse_args()
    MELODIC_FRAMES = align_frames(args.melodic_frames)

    if not os.path.exists(args.soundfont):
        sys.exit("gmbake: no such soundfont: %s" % args.soundfont)
    outdir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(outdir, exist_ok=True)

    jobs = []
    for prog in range(128):
        jobs.append(dict(kind=KIND_MELODIC, index=prog, channel=0,
                         program=prog, note=ROOT_NOTE,
                         hold=RENDER_HOLD_S, total=RENDER_TOTAL_S,
                         name="m%03d" % prog))
    for note in DRUM_NOTES:
        jobs.append(dict(kind=KIND_DRUM, index=note, channel=9,
                         program=None, note=note,
                         hold=DRUM_HOLD_S, total=DRUM_TOTAL_S,
                         name="d%03d" % note))

    workdir = args.keep_renders or tempfile.mkdtemp(prefix="gmbake-")
    os.makedirs(workdir, exist_ok=True)

    def render(job):
        mid = os.path.join(workdir, job["name"] + ".mid")
        wav = os.path.join(workdir, job["name"] + ".wav")
        with open(mid, "wb") as f:
            f.write(build_smf(job["channel"], job["program"], job["note"],
                              job["hold"], job["total"]))
        job["wav"] = wav
        job["ok"] = render_one(args.soundfont, mid, wav, args.gain)
        return job

    sys.stderr.write("gmbake: rendering %d samples with fluidsynth...\n" % len(jobs))
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = list(pool.map(render, jobs))

    entries = []
    failed = []
    silent = []
    total_bytes = 0
    err_num = 0.0
    err_den = 0.0
    sig_num = 0.0
    worst = (0.0, "")

    # Pass 1: condition everything and find the loudest sample in the set. The
    # gain cannot be decided per sample as it is encoded, because it depends on
    # the whole bank.
    sys.stderr.write("gmbake: conditioning...\n")
    prepared = []
    for job in jobs:
        if not job["ok"]:
            failed.append(job["name"])
            continue
        try:
            pcm = read_wav_mono(job["wav"])
        except Exception as exc:
            sys.stderr.write("gmbake: %s: %s\n" % (job["name"], exc))
            failed.append(job["name"])
            continue

        if pcm.size == 0 or float(np.abs(pcm).max()) < 1e-5:
            silent.append(job["name"])
            continue

        looped = job["kind"] == KIND_MELODIC and is_sustaining(pcm)
        loop_start = 0
        if job["kind"] == KIND_MELODIC:
            out, loop_start = prepare_melodic(pcm, looped)
        else:
            out = prepare_drum(pcm)

        frames = out.shape[0]
        assert frames % FRAME_ALIGN == 0, job["name"]
        if frames > KOS_MAX_FRAMES:
            frames = (KOS_MAX_FRAMES // FRAME_ALIGN) * FRAME_ALIGN
            out = out[:frames]
        prepared.append((job, out, looped, frames, loop_start))

    peaks_pre = [float(np.abs(o).max()) for _, o, _, _, _ in prepared]
    rms_pre = [float(np.sqrt((o.astype(np.float64) ** 2).mean()))
               for _, o, _, _, _ in prepared]
    gain = global_gain(peaks_pre, args.target_peak)

    # Pass 2: apply the one gain, then encode.
    sys.stderr.write("gmbake: encoding (%s), global gain %.3f...\n"
                     % (args.format, gain))
    peaks_post = []
    rms_post = []
    clipped = []
    for job, out, looped, frames, loop_start in prepared:
        out = (out * gain).astype(np.float32)
        peak = float(np.abs(out).max())
        peaks_post.append(peak)
        rms_post.append(float(np.sqrt((out.astype(np.float64) ** 2).mean())))
        if peak > 1.0:
            clipped.append(job["name"])

        if args.format == "adpcm":
            src = to_pcm16(out)
            blob = pcm_to_adpcm(src)
            back = adpcm_to_pcm(blob)
            diff = src.astype(np.float64) - back.astype(np.float64)
            err_num += float((diff ** 2).sum())
            err_den += float(src.shape[0])
            sig_num += float((src.astype(np.float64) ** 2).sum())
            rms = float(np.sqrt((diff ** 2).mean()))
            ref = float(np.sqrt((src.astype(np.float64) ** 2).mean())) or 1.0
            if rms / ref > worst[0]:
                worst = (rms / ref, job["name"])
        else:
            blob = to_pcm8s(out).tobytes()

        assert len(blob) % 32 == 0, job["name"]
        total_bytes += len(blob)

        entries.append(dict(kind=job["kind"], index=job["index"],
                            root=ROOT_NOTE if job["kind"] == KIND_MELODIC
                            else job["index"],
                            loop=1 if looped else 0, loop_start=loop_start,
                            rate=RATE, frames=frames, blob=blob, offset=0))

    entries.sort(key=lambda e: (e["kind"], e["index"]))
    offset = 0
    for e in entries:
        e["offset"] = offset
        offset += len(e["blob"])
    bank_bytes, data_off = write_bank(
        args.output, entries,
        FMT_ADPCM if args.format == "adpcm" else FMT_PCM8)

    n_parts = write_parts(args.output, args.part_bytes)

    n_mel = sum(1 for e in entries if e["kind"] == KIND_MELODIC)
    n_drum = sum(1 for e in entries if e["kind"] == KIND_DRUM)
    n_loop = sum(1 for e in entries if e["loop"])
    print("output      : %s" % args.output)
    print("format      : %s" % ("4-bit Yamaha ADPCM" if args.format == "adpcm"
                                else "8-bit signed PCM"))
    print("melodic     : %d" % n_mel)
    print("drum        : %d" % n_drum)
    print("looped      : %d" % n_loop)
    print("sample bytes: %d (%.1f KiB)" % (total_bytes, total_bytes / 1024.0))
    print("table       : %d bytes header+entries, data at %d"
          % (data_off, data_off))

    def db(v):
        return 20.0 * np.log10(v) if v > 0.0 else float("-inf")

    print("global gain : %.4f (%+.1f dB), one multiplier for the whole bank"
          % (gain, db(gain)))
    print("peak  before: max %.4f (%+.1f dBFS)  median %.4f  min %.4f"
          % (max(peaks_pre), db(max(peaks_pre)),
             np.median(peaks_pre), min(peaks_pre)))
    print("peak  after : max %.4f (%+.1f dBFS)  median %.4f  min %.4f"
          % (max(peaks_post), db(max(peaks_post)),
             np.median(peaks_post), min(peaks_post)))
    print("rms   before: median %.4f (%+.1f dBFS)"
          % (np.median(rms_pre), db(np.median(rms_pre))))
    print("rms   after : median %.4f (%+.1f dBFS)"
          % (np.median(rms_post), db(np.median(rms_post))))
    print("balance     : %.1f dB spread across the bank, unchanged by the gain"
          % (db(max(peaks_pre)) - db(min(peaks_pre))))
    print("clipping    : %s" % (" ".join(clipped) if clipped else "none"))
    print("parts       : %d x %d bytes" % (n_parts, args.part_bytes)
          if n_parts else "parts       : none (single file)")
    print("total       : %d bytes (%.2f MiB)"
          % (bank_bytes, bank_bytes / 1048576.0))
    if args.format == "adpcm" and err_den:
        rms = np.sqrt(err_num / err_den)
        print("round-trip  : RMS error %.1f LSB of 32768 (%.3f%% FS), "
              "SNR %.1f dB, worst %s"
              % (rms, 100.0 * rms / 32768.0,
                 10.0 * np.log10(sig_num / err_num), worst[1]))
    if silent:
        print("silent      : %s" % " ".join(silent))
    if failed:
        print("failed      : %s" % " ".join(failed))
    if not args.keep_renders:
        for job in jobs:
            for ext in (".mid", ".wav"):
                p = os.path.join(workdir, job["name"] + ext)
                if os.path.exists(p):
                    os.unlink(p)
        os.rmdir(workdir)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
