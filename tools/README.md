# Host tools

These tools run on the development machine and produce assets for the
Dreamcast build.

## Hardware capture

`dccheck.sh` loads a page through dc-load, captures the displayed frame, and
compares it with the host SWF renderer.

```sh
tools/dccheck.sh swf/t_rect.html
tools/dccheck.sh --ip 192.0.2.10 test.html
```

Relative paths become `file:///pc/<path>`. Captures and comparison images are
written below `out/dc/`. The script requires `dc-tool-ip` and uses `-l` for
compatibility with older KallistiOS dc-load support.

## Cursor baker

`curbake` converts Windows `.cur` and `.ani` files into POPSurf's little-endian
`.psc` cursor bundle.

```sh
make -C tools
tools/curbake input-directory output.psc
```

Files are assigned to CSS cursor roles using their conventional Windows cursor
names. Unrecognized files are skipped. Entries are sorted so repeated builds
are reproducible.

The `.psc` header is 16 bytes: `PSC1`, version, role count, frame count, and
the offsets of the role and frame tables. Role entries contain a role ID and
frame range. Frame entries contain dimensions, hotspot, delay, data offset,
and data length. Pixel data is ARGB4444.

## General MIDI bank baker

`gmbake.py` renders a SoundFont into the compact `.psb` bank used by POPSurf's
AICA MIDI synthesizer.

```sh
tools/gmbake.py soundfont.sf2 dist/gmbank.psb
```

It requires Python, NumPy, and FluidSynth. The default build contains the 128
General MIDI programs and drum notes 35–81. Samples are rendered as mono PCM8,
trimmed, looped where appropriate, and stored in a little-endian `PSGM` file.

Use `--help` for tuning and split-output options.
