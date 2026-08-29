/* Dreamcast voice backend: AICA channel registers, driven from the SH-4.
 *
 * The AICA has 64 hardware PCM voices with per-voice pitch, volume, pan and
 * looping, and decodes 4-bit Yamaha ADPCM without help. Sample data lives in
 * 2MB of SPU RAM that is separate from the 16MB the browser runs in, so a
 * sample bank costs nothing against the page budget.
 *
 * Why this talks to the registers rather than to KOS's snd_sfx API:
 *
 * snd_sfx posts commands into a queue in SPU RAM that the AICA's ARM7
 * co-processor is meant to pick up and act on. That indirection exists so the
 * SH-4 does not have to touch the G2 bus per note. But it means the whole API
 * is fire-and-forget: if the ARM is not executing, snd_sfx_load_raw_buf still
 * returns a valid handle, snd_sfx_play_ex still returns a channel number, and
 * the channel registers are simply never written. Silence, with every return
 * value saying success.
 *
 * That is exactly what happens under flycast 2.6, verified directly: the
 * firmware uploads correctly (first word reads back ea00002d), the ARM reset
 * bit clears and the write sticks, and yet the driver's millisecond clock at
 * SPU offset 0x021000 never leaves zero and channel 0 stays unprogrammed.
 *
 * Writing the registers ourselves removes the ARM from the picture entirely.
 * It works wherever the AICA does, costs a handful of G2 writes per note-on -
 * which at MIDI note rates is nothing - and gives exact control over voice
 * allocation, which a sequencer wants anyway.
 *
 * The register sequence below is a port of KOS's own arm/aica.c aica_play().
 * Only the transport changes, from ARM-side stores to SH-4 G2 writes.
 *
 * Every one of those writes is taken under g2_lock(). The AICA and the
 * Broadband Adapter sit on the same G2 bus, and g2_lock() suspends G2 DMA -
 * the BBA's included - while the SH-4 has the bus. Without it a note-on
 * issued during a network transfer collides with the adapter's DMA. That is
 * not a theoretical race: at sixty notes a second, plus a controller update
 * touching every sounding voice, a page fetching a megabyte is doing both at
 * once continuously. KOS takes the same lock in spu_reset_chans for the same
 * reason.
 */
#include "ps_voice.h"
#include "ps_audio.h"   /* PS_AUDIO_VOICES */
#include "ps_config.h"

#include <kos.h>
#include <dc/g2bus.h>
#include <dc/spu.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>

#include <stdlib.h>
#include <string.h>

#define AICA_REGS      0xa0700000
#define AICA_MASTERVOL (AICA_REGS + 0x2800)
#define CHNREG(c, r)   (AICA_REGS + 0x80 * (c) + (r))

/* Position readback. The AICA reports one channel at a time: the slot number
 * goes into the monitor field of 0x280c and the current address then appears
 * at 0x2814, in frames from the sample's start address. KOS's ARM-side driver
 * reads the pair the same way - the only difference here is that the slot
 * select arrives as a 32-bit G2 write instead of a byte store, for the reason
 * spelled out above CHNREG16. */
#define AICA_MONITOR   (AICA_REGS + 0x280c)
#define AICA_CURADDR   (AICA_REGS + 0x2814)
#define AICA_MSLC_SHIFT 8

/* Channel register offsets, named so the play sequence reads as intent
 * rather than as arithmetic. */
#define CR_PLAY     0x00   /* key on/off, sample format, address high bits */
#define CR_SA_LOW   0x04
#define CR_LOOPST   0x08
#define CR_LOOPEND  0x0c
#define CR_AEG      0x10   /* amplitude envelope: attack / decay rates */
#define CR_AEG2     0x14   /* amplitude envelope: decay level / release rate */
#define CR_FNS_OCT  0x18   /* pitch */
#define CR_PAN      0x24
#define CR_DISDL    0x25   /* direct send level */
#define CR_LPF      0x28
#define CR_VOL      0x29

/* Envelope rates are 0..31 with 31 the fastest. A fast attack is right for
 * sampled instruments - the attack is already in the sample - but the stock
 * fastest release cuts a note dead, and at the ~60 notes a second a busy MIDI
 * produces that reads as a continuous click track rather than as music. A
 * moderate release lets each note-off ramp out over some tens of
 * milliseconds instead. */
#define AEG_ATTACK  0x1f
#define AEG_RELEASE 0x0a

/* AICA master volume is 4 bits. Running it wide open leaves no headroom, and
 * a sequencer summing up to PS_AUDIO_VOICES notes into the same mix clips
 * hard on every dense passage - which sounds like harsh distortion rather
 * than like something being too loud. Backing off gives the mixer room for
 * full polyphony; per-note dynamics are unaffected because velocity is
 * applied per channel. Two steps is enough now that channel volume and
 * expression scale each voice as the file intended - before those were
 * honoured every part ran at full level and needed far more headroom. */
#define MASTER_VOL  0x0d

/* Writes one of the AICA's packed 16-bit registers as a single 32-bit access.
 *
 * KOS's aica.c writes these register halves as bytes, and that is correct
 * there: it runs on the ARM7, which sits directly on the AICA. From the SH-4
 * the same stores cross the G2 bus, and byte writes do not survive the trip.
 * Confirmed on hardware - after a g2_write_8 of 0x0f, DISDL read back as 0x00,
 * while every 32-bit write in the same sequence landed intact.
 *
 * That one dropped write is worth the whole comment: DISDL is the direct send
 * level, so losing it leaves a voice with the right sample, pitch, format and
 * volume, routed nowhere. Silence, with every register you would think to
 * check looking perfectly correct.
 *
 * It never showed up under flycast, which does not model the restriction and
 * accepts byte writes the hardware discards. Pair the halves and write the
 * word: SH-4 is little-endian, so the low byte lands at the lower address -
 * DIPAN at 0x24 with DISDL at 0x25, filter at 0x28 with TL at 0x29. */
#define CHNREG16(c, r, v) g2_write_32(CHNREG((c), (r)), (uint32_t)(v))

#define CR_KEYONEX  0x8000
#define CR_KEYONB   0x4000
#define CR_LOOPFLAG 0x0200

/* Linear 0..255 to the AICA's logarithmic attenuation, where 0 is loudest.
 * Copied from KOS's driver so a given volume sounds identical to what the
 * stock path would have produced. */
static const uint8_t ps_logs[256] = {
    255, 127, 111, 102, 95, 90, 86, 82, 79, 77, 74, 72, 70, 68, 66, 65,
    63, 62, 61, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 50, 49, 48,
    47, 47, 46, 45, 45, 44, 43, 43, 42, 42, 41, 41, 40, 40, 39, 39,
    38, 38, 37, 37, 36, 36, 35, 35, 34, 34, 34, 33, 33, 33, 32, 32,
    31, 31, 31, 30, 30, 30, 29, 29, 29, 28, 28, 28, 27, 27, 27, 27,
    26, 26, 26, 25, 25, 25, 25, 24, 24, 24, 24, 23, 23, 23, 23, 22,
    22, 22, 22, 21, 21, 21, 21, 20, 20, 20, 20, 20, 19, 19, 19, 19,
    18, 18, 18, 18, 18, 17, 17, 17, 17, 17, 17, 16, 16, 16, 16, 16,
    15, 15, 15, 15, 15, 15, 14, 14, 14, 14, 14, 14, 13, 13, 13, 13,
    13, 13, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11, 11, 11, 10,
    10, 10, 10, 10, 10, 10, 9, 9, 9, 9, 9, 9, 9, 8, 8, 8,
    8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6,
    6, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* Hardware channels reserved for us, in the order they were handed out.
 * Reserving through KOS's allocator rather than hardcoding 0..N means an
 * embedding game can keep using snd_sfx for its own audio without us
 * stamping on its channels - which matters, since the whole point of
 * libpopsurf is running inside someone else's game. */
static int  ps_chan[PS_AUDIO_VOICES];
static int  ps_nchan;
static int  ps_ready;

int ps_voice_init(int want)
{
    int i;

    if(ps_ready)
        return ps_nchan;

    if(want > PS_AUDIO_VOICES)
        want = PS_AUDIO_VOICES;

    /* Brings up SPU RAM and its allocator. It also uploads KOS's sound driver
     * and releases the ARM, which we then undo below. */
    if(snd_init() < 0)
        return 0;

    /* Put the ARM straight back into reset.
     *
     * We program the channel registers ourselves from the SH-4, and the ARM's
     * driver services those same registers on a continuous loop. With both
     * running, every voice we key gets overwritten on the driver's next pass:
     * the sequencer reports notes played and the speakers stay silent.
     *
     * This never showed up under flycast because there the ARM does not
     * execute at all - which is the whole reason this backend bypasses
     * snd_sfx in the first place (see the header comment). The emulator was
     * quietly providing the exclusivity that real hardware does not, so the
     * bug could only ever appear on a real Dreamcast.
     *
     * Nothing here needs the ARM: snd_mem_malloc and snd_sfx_chn_alloc are
     * SH-4 side bookkeeping, and the AICA's channel playback hardware runs
     * independently of the co-processor. This also resets all channels, so it
     * has to happen before any of them is programmed.
     *
     * Off by default: tried on hardware and it changed nothing, so the ARM is
     * not in fact stealing our channels. Kept behind a flag because the
     * reasoning still holds if a later symptom points back this way, and
     * because KOS's own snd_sfx path needs the ARM alive to be compared
     * against. */
#ifdef PS_AICA_ARM_RESET
    spu_disable();
#endif

    for(i = 0; i < want; i++) {
        int c = snd_sfx_chn_alloc();

        if(c < 0)
            break;
        ps_chan[i] = c;
    }
    ps_nchan = i;
    ps_ready = 1;

    {
        g2_lock_scoped();
        g2_fifo_wait();
        /* Written whole rather than read-modify-write. Reading an AICA
         * register back over G2 is not as dependable as it looks, and a bad
         * read here would fold garbage into the bits we are not touching -
         * including mono and the output enables. KOS's own init sets this
         * register absolutely for the same reason. */
        g2_write_32(AICA_MASTERVOL, MASTER_VOL);
    }

    return ps_nchan;
}

void ps_voice_shutdown(void)
{
    int i;

    if(!ps_ready)
        return;

    for(i = 0; i < ps_nchan; i++) {
        ps_voice_stop(i);
        snd_sfx_chn_free(ps_chan[i]);
    }
    ps_nchan = 0;
    ps_ready = 0;
}

ps_smp ps_voice_upload(const void *data, size_t len)
{
    uint32_t  addr;
    size_t    padded = (len + 31) & ~(size_t)31;
    void     *bounce = NULL;
    const void *src  = data;

    if(!ps_ready || !data || !len)
        return PS_SMP_NONE;

    addr = snd_mem_malloc(padded);
    if(!addr)
        return PS_SMP_NONE;

    /* SPU transfers want a 32-byte aligned source and a whole number of
     * 32-byte units. Callers that already satisfy that - which the sample
     * bank does, since it is relocated to an aligned allocation and its
     * offsets are multiples of 32 - skip the copy entirely. */
    if(((uintptr_t)data & 31) != 0 || padded != len) {
        bounce = memalign(32, padded);
        if(!bounce) {
            snd_mem_free(addr);
            return PS_SMP_NONE;
        }
        memcpy(bounce, data, len);
        if(padded > len)
            memset((char *)bounce + len, 0, padded - len);
        src = bounce;
    }

    /* Store-queue transfer: synchronous, so the source may be freed as soon
     * as it returns. The DMA variant would not give that guarantee. */
    spu_memload_sq(addr, src, padded);

    free(bounce);
    return (ps_smp)addr;
}

ps_smp ps_voice_alloc(size_t len)
{
    uint32_t addr;
    size_t   padded = (len + 31) & ~(size_t)31;

    if(!ps_ready || !len)
        return PS_SMP_NONE;

    addr = snd_mem_malloc(padded);
    if(!addr)
        return PS_SMP_NONE;

    /* Silence, not whatever the last owner left. A stream keys its voice on
     * the ring before the first refill has necessarily landed, and stale
     * sample data played at full volume is not a quiet failure. */
    spu_memset(addr, 0, padded);
    return (ps_smp)addr;
}

int ps_voice_write(ps_smp s, uint32_t off, const void *data, size_t len)
{
    if(!ps_ready || s == PS_SMP_NONE || !data || !len)
        return -1;

    /* The store-queue path moves 32 bytes at a time from a 32-byte aligned
     * source. A stream refills in fixed chunks that are multiples of that by
     * construction, so failing here means a caller bug rather than a case to
     * bounce through a temporary buffer. */
    if((off & 31) || (len & 31) || ((uintptr_t)data & 31))
        return -1;

    spu_memload_sq((uintptr_t)s + off, data, len);
    return 0;
}

/* Startup diagnostic: does the backend reach the hardware at all?
 *
 * When a tune is silent there are two entirely different faults hiding behind
 * one symptom - the sequencer choosing bad parameters, or the voice backend
 * never reaching the AICA - and a note counter cannot tell them apart. It
 * reported thousands of notes played while the speakers stayed silent.
 *
 * So this splits the question. It uploads a fixed tone, reads SPU RAM back to
 * prove the sample data arrived, and keys it with constants rather than
 * anything the sequencer computed. If it sounds, the backend is sound and the
 * bug is above it. If it is silent, nothing above matters yet. And if the
 * readback mismatches, the fault is in the transfer, not the registers at all. */
void ps_voice_selftest(void)
{
    enum { RATE = 22050, HZ = 440, PERIOD = RATE / HZ, FRAMES = PERIOD * 20 };
    int8_t *pcm = memalign(32, FRAMES);
    int8_t  back[32];
    ps_smp  s;
    int     i, bad = 0;

    if(!ps_ready) {
        printf("popsurf: selftest skipped, no voices\n");
        return;
    }
    if(!pcm)
        return;

    /* A square wave: unmistakable, and a whole number of periods so the loop
     * joins cleanly without needing a loop point. */
    for(i = 0; i < FRAMES; i++)
        pcm[i] = ((i % PERIOD) < PERIOD / 2) ? 100 : -100;

    s = ps_voice_upload(pcm, FRAMES);
    if(s == PS_SMP_NONE) {
        printf("popsurf: selftest upload failed\n");
        free(pcm);
        return;
    }

    /* Proves the store-queue transfer actually landed in sound RAM. */
    spu_memread(back, (uintptr_t)s, sizeof back);
    for(i = 0; i < (int)sizeof back; i++)
        if(back[i] != pcm[i])
            bad++;

    printf("popsurf: selftest sample at spu 0x%08x, readback %s (%d/%d differ)\n",
           (unsigned)s, bad ? "MISMATCH" : "ok", bad, (int)sizeof back);

    /* Tone A: our own register programming. */
    ps_voice_play(0, s, PS_FMT_PCM8, 1, 0, FRAMES, RATE, 255, 128);

    /* Read the hardware back rather than trusting that the writes landed.
     *
     * Both playback paths being silent means the fault is below both of them,
     * so the question is no longer "is our sequence right" but "do these
     * writes reach the chip at all". A register that reads back as what we
     * wrote proves the G2 path; one that reads back zero or garbage says the
     * writes are being dropped, which is a different bug entirely and not one
     * that lives in this file. */
    {
        int      ch = ps_chan[0];
        uint32_t mvol, play, fns;
        uint8_t  vol, disdl, pan;

        g2_lock_scoped();
        g2_fifo_wait();
        mvol  = g2_read_32(AICA_MASTERVOL);
        play  = g2_read_32(CHNREG(ch, CR_PLAY));
        fns   = g2_read_32(CHNREG(ch, CR_FNS_OCT));
        g2_fifo_wait();
        vol   = g2_read_8(CHNREG(ch, CR_VOL));
        disdl = g2_read_8(CHNREG(ch, CR_DISDL));
        pan   = g2_read_8(CHNREG(ch, CR_PAN));
        g2_fifo_wait();

        printf("popsurf: selftest regs ch%d play=%04x fns=%04x vol=%02x "
               "disdl=%02x pan=%02x mvol=%04x\n",
               ch, (unsigned)play, (unsigned)fns, vol, disdl, pan,
               (unsigned)mvol);
    }

    printf("popsurf: selftest TONE A (ours) playing 3s...\n");
    thd_sleep(3000);
    ps_voice_kill(0);
    ps_voice_release(s);

    /* Tone B: the same samples through KOS's stock path, which posts commands
     * to the ARM instead of touching registers from here.
     *
     * This is the measurement that matters. Both tones carry identical data,
     * so whichever is silent names the faulty half: if B sounds and A does
     * not, the fault is in our register writes; if neither sounds, the fault
     * is below both of us - in the AICA's common setup or the output path -
     * and no amount of studying our channel code will find it. */
    {
        sfxhnd_t h = snd_sfx_load_raw_buf((char *)pcm, FRAMES, RATE, 8, 1);

        if(h == SFXHND_INVALID) {
            printf("popsurf: selftest TONE B load failed\n");
        }
        else {
            snd_sfx_play(h, 255, 128);
            printf("popsurf: selftest TONE B (KOS snd_sfx) playing 3s...\n");
            thd_sleep(3000);
            snd_sfx_unload(h);
        }
    }

    free(pcm);
    printf("popsurf: selftest done\n");
}

void ps_voice_release(ps_smp s)
{
    if(s != PS_SMP_NONE)
        snd_mem_free((uint32_t)s);
}

/* Every register a voice needs except the key itself, returning the play
 * control word the caller must write to start it.
 *
 * Shared by the note path and the streaming path, which differ only in when
 * the voice is keyed. The caller holds the G2 lock. */
static uint32_t program_voice(int ch, ps_smp s, int fmt, int loop,
                              uint32_t loop_start, uint32_t frames,
                              uint32_t freq, int vol, int pan)
{
    uint32_t freq_base = 5644800, freq_lo, playctl;
    int      freq_hi = 7, hwpan;

    /* Pitch is a floating point field: freq = 44100 * 2^oct * (1 + fns/1024).
     * Normalise the exponent down until the mantissa fits. */
    while(freq < freq_base && freq_hi > -8) {
        freq_base >>= 1;
        --freq_hi;
    }
    freq_lo = (freq << 10) / freq_base;

    /* Hardware pan is a 5-bit magnitude plus a side bit, not a linear scale. */
    hwpan = (pan == 0x80) ? 0
          : (pan <  0x80) ? (0x10 | ((0x7f - pan) >> 3))
                          : ((pan - 0x80) >> 3);

    if(vol < 0)   vol = 0;
    if(vol > 255) vol = 255;

    /* LSA past the attack, so the transient plays once and only the sustain
     * repeats. LEA bounds playback whether or not the sample loops - it is the
     * end of the data, and only the loop flag decides what happens there. */
    g2_write_32(CHNREG(ch, CR_LOOPST),  loop_start & 0xffff);
    g2_write_32(CHNREG(ch, CR_LOOPEND), frames & 0xffff);
    g2_write_32(CHNREG(ch, CR_FNS_OCT), (freq_hi << 11) | (freq_lo & 1023));
    g2_fifo_wait();

    /* Paired into 32-bit accesses. See the note above CHNREG16 - byte writes
     * to these registers are dropped crossing the G2 bus, and a lost DISDL is
     * silence with every other register visibly correct. */
    CHNREG16(ch, CR_PAN, (0x0f << 8) | (uint8_t)hwpan);
    CHNREG16(ch, CR_LPF, (ps_logs[vol] << 8) | 0x24);   /* TL | filter off */
    g2_fifo_wait();

    /* Attack fast, no decay so the note sustains at velocity level, and a
     * gentle release so key-off ramps out instead of clicking. */
    g2_write_32(CHNREG(ch, CR_AEG),  AEG_ATTACK);
    g2_write_32(CHNREG(ch, CR_AEG2), AEG_RELEASE);
    g2_write_32(CHNREG(ch, CR_SA_LOW), (uint32_t)s & 0xffff);
    g2_fifo_wait();

    playctl = ((uint32_t)fmt << 7) | ((uint32_t)s >> 16);
    if(loop)
        playctl |= CR_LOOPFLAG;

    return playctl;
}

void ps_voice_play(int slot, ps_smp s, int fmt, int loop, uint32_t loop_start,
                   uint32_t frames, uint32_t freq, int vol, int pan)
{
    uint32_t playctl;
    int      ch;

    if(!ps_ready || slot < 0 || slot >= ps_nchan || s == PS_SMP_NONE)
        return;

    ch = ps_chan[slot];

    g2_lock_scoped();
    g2_fifo_wait();

    /* Key off before reprogramming, or the channel keeps sounding the old
     * sample through the address change. */
    g2_write_32(CHNREG(ch, CR_PLAY),
                (g2_read_32(CHNREG(ch, CR_PLAY)) & ~CR_KEYONB) | CR_KEYONEX);
    g2_fifo_wait();

    playctl = program_voice(ch, s, fmt, loop, loop_start, frames, freq, vol,
                            pan);

    g2_write_32(CHNREG(ch, CR_PLAY), CR_KEYONEX | CR_KEYONB | playctl);
    g2_fifo_wait();
}

void ps_voice_arm(int slot, ps_smp s, int fmt, int loop, uint32_t loop_start,
                  uint32_t frames, uint32_t freq, int vol, int pan)
{
    uint32_t playctl;
    int      ch;

    if(!ps_ready || slot < 0 || slot >= ps_nchan || s == PS_SMP_NONE)
        return;

    ch = ps_chan[slot];

    g2_lock_scoped();
    g2_fifo_wait();

    /* Clear the key bit but do not pulse KYONEX here, which is the whole
     * difference from ps_voice_play. KYONEX is a global trigger: any channel
     * armed a moment ago would start on it, and arming the second half of a
     * stereo pair would then be the thing that started the first half. */
    g2_write_32(CHNREG(ch, CR_PLAY),
                g2_read_32(CHNREG(ch, CR_PLAY)) & ~(CR_KEYONB | CR_KEYONEX));
    g2_fifo_wait();

    playctl = program_voice(ch, s, fmt, loop, loop_start, frames, freq, vol,
                            pan);

    g2_write_32(CHNREG(ch, CR_PLAY), CR_KEYONB | playctl);
    g2_fifo_wait();
}

void ps_voice_key(int slot)
{
    int ch;

    if(!ps_ready || slot < 0 || slot >= ps_nchan)
        return;

    ch = ps_chan[slot];

    g2_lock_scoped();
    g2_fifo_wait();
    g2_write_32(CHNREG(ch, CR_PLAY),
                g2_read_32(CHNREG(ch, CR_PLAY)) | CR_KEYONEX);
    g2_fifo_wait();
}

int ps_voice_pos(int slot)
{
    uint32_t ca;
    int      ch;

    if(!ps_ready || slot < 0 || slot >= ps_nchan)
        return -1;

    ch = ps_chan[slot];

    g2_lock_scoped();
    g2_fifo_wait();
    /* Written whole rather than merged into what is already there: the low
     * half of this register is the MIDI input buffer, which is read-only and
     * on a stock console never has anything in it, and a read-modify-write
     * would put a bad G2 read into the slot select. */
    g2_write_32(AICA_MONITOR, (uint32_t)(ch & 0x3f) << AICA_MSLC_SHIFT);
    g2_fifo_wait();
    ca = g2_read_32(AICA_CURADDR);
    g2_fifo_wait();

    return (int)(ca & 0xffff);
}

/* Only the two level registers are touched; the play control register is
 * left alone so the note keeps sounding from wherever it had reached. */
void ps_voice_set_level(int slot, int vol, int pan)
{
    int ch, hwpan;

    if(!ps_ready || slot < 0 || slot >= ps_nchan)
        return;

    ch = ps_chan[slot];

    if(vol < 0)   vol = 0;
    if(vol > 255) vol = 255;

    hwpan = (pan == 0x80) ? 0
          : (pan <  0x80) ? (0x10 | ((0x7f - pan) >> 3))
                          : ((pan - 0x80) >> 3);

    g2_lock_scoped();
    g2_fifo_wait();
    /* Both halves of each word go out together, so DISDL and the filter are
     * restated rather than left to survive on their own. They hold the values
     * ps_voice_play set; writing them again costs nothing and means an
     * expression change can never quietly unroute a sounding voice.
     *
     * This path matters as much as keying a note does: a MIDI file may send
     * hundreds of CC11 events to shape one phrase, so every one of these was
     * a dropped write before. */
    CHNREG16(ch, CR_PAN, (0x0f << 8) | (uint8_t)hwpan);
    CHNREG16(ch, CR_LPF, (ps_logs[vol] << 8) | 0x24);
    g2_fifo_wait();
}

void ps_voice_kill(int slot)
{
    int ch;

    if(!ps_ready || slot < 0 || slot >= ps_nchan)
        return;

    ch = ps_chan[slot];

    g2_lock_scoped();
    g2_fifo_wait();
    /* Fastest release, then key off: the envelope reaches zero at once
     * instead of over the tens of milliseconds a musical release takes. */
    g2_write_32(CHNREG(ch, CR_AEG2), 0x1f);
    g2_write_32(CHNREG(ch, CR_PLAY),
                (g2_read_32(CHNREG(ch, CR_PLAY)) & ~CR_KEYONB) | CR_KEYONEX);
    g2_fifo_wait();
}

void ps_voice_stop(int slot)
{
    int ch;

    if(!ps_ready || slot < 0 || slot >= ps_nchan)
        return;

    ch = ps_chan[slot];

    g2_lock_scoped();
    g2_fifo_wait();
    g2_write_32(CHNREG(ch, CR_PLAY),
                (g2_read_32(CHNREG(ch, CR_PLAY)) & ~CR_KEYONB) | CR_KEYONEX);
    g2_fifo_wait();
}
