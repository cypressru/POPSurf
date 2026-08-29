# PVR submission and BBA transfers cannot overlap

Minimal reproducer for a fault that looked, for a long time, like an emulator
bug or a memory problem in the browser. It is neither: driving the tile
accelerator while the Broadband Adapter is moving bulk data kills the machine.

## What it does

Downloads ~2MB over HTTP from a local server while optionally rendering an
empty PVR scene every frame. Nothing else: no audio, no allocation beyond one
buffer, no browser code.

## Results

Measured against a 2102793-byte file, flycast 2.6, KOS 2.3.0:

| configuration                              | got       | outcome        |
|--------------------------------------------|-----------|----------------|
| download on main thread, no PVR            | 2102793   | completed      |
| download on worker thread, no PVR          | 2102793   | completed      |
| download on worker thread + PVR every frame|  262813   | host SIGSEGV   |
| ... with PVR throttled to 4fps             |       0   | host SIGSEGV   |
| ... with PVR suspended during the transfer | 1837293   | SH4 exception  |

The first two lines are the control: the transfer itself is fine, and putting
it on a second thread changes nothing. Adding PVR submission is what breaks
it, and throttling does not help - it is not a matter of how much traffic,
only whether any is submitted while the adapter is busy.

Suspending submission entirely gets seven times further but still failed once,
so serialising is a large mitigation rather than a cure.

## Why it matters

The browser renders every frame and fetches a multi-megabyte soundbank, so it
does both at once by construction. That is why the crash looked like it was
about download size, then about chunking, then about memory: each of those
changed how long the two overlapped.

Worth confirming on hardware before concluding anything about a real
Dreamcast - if the adapter and the PVR coexist there, this is an emulation
artefact and only the mitigation matters.

## Running

Serve a ~2MB file at HOST:PORT/PATH (see the defines in main.c), then:

    make && mkdcdisc -e nettest.elf -o nettest.cdi && flycast nettest.cdi
