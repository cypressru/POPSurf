#!/usr/bin/env bash
# dccheck.sh - send the browser to a page on real hardware and bring the frame
# back, with nobody at the console.
#
#   tools/dccheck.sh swf/t_rect.html          load it, capture, compare
#   tools/dccheck.sh --ip 192.0.2.10 test.html explicit console address
#   tools/dccheck.sh -n rect swf/t_rect.html  name the output files
#   tools/dccheck.sh -f 1 swf/t_clipn.html    hold the movie on frame 1
#   tools/dccheck.sh --arg swfmask=vol ...    any other bootargs setting
#   tools/dccheck.sh --no-build swf/t_rect.html
#
# A path with no scheme is taken as a file under the mapped directory, so
# `swf/t_rect.html` means file:///pc/swf/t_rect.html and is the same file the
# host suite renders. Everything lands in out/dc/.
#
# Why this exists: every hardware check until now was a person with a d-pad
# describing what they saw, and in one session that produced two wrong
# diagnoses - a test font whose only glyph is a filled square reported as "text
# renders as green squares", and gradient testbeds that contain a deliberate
# hard step reported as "gradients are two blocks". Both were correct renders.
# Both would have been settled instantly by putting the console's pixels beside
# the software renderer's, which is what this does.
#
# The browser side is shell/ps_probe.c: KOS drops dcload's argv, so the address
# and the capture path travel in cd/.bootargs, which this writes and clears.
set -euo pipefail
cd "$(dirname "$0")/.."

IP="${DC_IP:-}"
MAPDIR="cd"
ELF="popsurf.elf"
OUTDIR="out/dc"
NAME=""
URL=""
SWF=""
BG=""
SETTLE=1200
TIMEOUT=25000
RUN_SECS=180
BUILD=1
COMPARE=1

# Which frame of the movie both sides are to show.
#
# Not a refinement: a movie plays on the console and does not on the host, so
# without this the console shows whichever frame 1200ms of settling reached and
# the reference shows frame zero. On a six-frame testbed built to give a
# different answer on every frame that is not a small error - it is a different
# picture, and it reads exactly like a rendering fault. The console is told the
# same number swfrender is.
FRAME=0

# Anything else the run wants to say to the browser, one --arg key=value each.
#
# The alternative was an option per setting, and the settings that matter are
# the ones nobody has thought of yet: separating a hardware behaviour into its
# parts takes several runs of one build differing in one thing, and the thing
# changes with the question. ps_probe names what it accepts and ignores the
# rest by name, so a typo here is a printed line rather than a silent run.
ARGS=()

usage() {
    sed -n '2,26p' "$0"
    exit "${1:-2}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --ip)        IP="$2"; shift ;;
        -n|--name)   NAME="$2"; shift ;;
        -s|--swf)    SWF="$2"; shift ;;
        -f|--frame)  FRAME="$2"; shift ;;
        --arg)       ARGS+=("$2"); shift ;;
        -b|--bg)     BG="$2"; shift ;;
        --settle)    SETTLE="$2"; shift ;;
        --timeout)   TIMEOUT="$2"; shift ;;
        --run-secs)  RUN_SECS="$2"; shift ;;
        --no-build)  BUILD=0 ;;
        --no-compare) COMPARE=0 ;;
        -h|--help)   usage 0 ;;
        -*)          echo "unknown option: $1" >&2; usage ;;
        *)           URL="$1" ;;
    esac
    shift
done

[ -n "$URL" ] || usage
[ -n "$IP" ] || {
    echo "set DC_IP or pass --ip with the Dreamcast address" >&2
    exit 2
}

# A path becomes an address under the mapped directory. The browser sees the
# host's cd/ as /cd's stand-in at /pc, so the same file the host suite reads is
# the file the console reads - no second copy to keep in step.
PAGE="$URL"
case "$URL" in
    *://*) ;;
    /*)    PAGE="file://$URL" ;;
    *)     PAGE="file:///pc/$URL" ;;
esac

# The name carries what makes the run different, so two runs of one page do not
# land on one another. A capture and its diff map are the evidence, and the
# first thing that happened when the same page was captured twice was that the
# second overwrote the first and the coordinates of the first were gone.
if [ -z "$NAME" ]; then
    NAME="$(basename "${URL%%\?*}")"
    NAME="${NAME%.html}"
    NAME="${NAME%.htm}"
    [ -n "$NAME" ] || NAME="shot"
    [ "$FRAME" = 0 ] || NAME="$NAME-f$FRAME"
    for a in ${ARGS+"${ARGS[@]}"}; do
        NAME="$NAME-${a#*=}"
    done
fi

# One tool at a time. A second dc-tool session silently fails to take the
# socket and uploads nothing, which reads as "the change did not work" and has
# cost this project a dozen power cycles. Refused rather than killed: the
# console is the user's, and a session that is mid-upload must not be shot.
for t in dc-tool-ip kos-tool; do
    if pgrep -x "$t" > /dev/null 2>&1; then
        echo "!! $t is already running. Only one dcload session at a time;" >&2
        echo "   wait for it, or kill it with: killall -9 $t" >&2
        exit 1
    fi
done

TOOL="${DC_TOOL:-}"
if [ -z "$TOOL" ]; then
    if command -v dc-tool-ip > /dev/null; then TOOL=dc-tool-ip
    elif command -v kos-tool > /dev/null; then TOOL=kos-tool
    else echo "!! neither dc-tool-ip nor kos-tool is on PATH" >&2; exit 1
    fi
fi

if [ "$BUILD" -eq 1 ]; then
    echo ">>> building"
    make -j8 > /dev/null
fi
[ -f "$ELF" ] || { echo "!! no $ELF; drop --no-build" >&2; exit 1; }

# The capture has to land inside the mapped directory, because that directory
# is the only thing the console can write to - /pc is that directory and
# nothing else. Dot-prefixed and moved out afterwards so the disc's own
# contents stay exactly what gets burned.
SHOTDIR="$MAPDIR/.shots"
mkdir -p "$SHOTDIR" "$OUTDIR"
rm -f "$SHOTDIR/$NAME.ppm"

# Written every run, cleared after: an empty file is "behave normally", so a
# later manual run cannot inherit last night's address.
cat > "$MAPDIR/.bootargs" <<EOF
# written by tools/dccheck.sh - cleared when it finishes
url=$PAGE
shot=/pc/.shots/$NAME.ppm
settle=$SETTLE
timeout=$TIMEOUT
frame=$FRAME
exit=1
EOF
for a in ${ARGS+"${ARGS[@]}"}; do
    echo "$a" >> "$MAPDIR/.bootargs"
done

LOG="$OUTDIR/$NAME.log"
echo ">>> $PAGE -> $SHOTDIR/$NAME.ppm  (console $IP, log $LOG)"

# -l is not optional on this KOS checkout: fs_dclsocket predates KallistiOS
# PR #1451 and hangs against the packet size dc-tool-ip 2.x sends by default,
# which looks exactly like the program crashing in main() - black screen, no
# console output, no exception. KOS issue #1092. See the project memory note
# dcload-needs-legacy-payload-flag, and BBALL2027/dcload.sh.
#
# -k escalates to KILL: kos-tool shrugs off SIGTERM and a survivor holds the
# GDB relay port, so the next run uploads nothing.
set +e
timeout -k 5 "$RUN_SECS" "$TOOL" -t "$IP" -l -m "$MAPDIR" -x "$ELF" 2>&1 \
    | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e

: > "$MAPDIR/.bootargs"

if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "!! the run did not end within ${RUN_SECS}s and was killed."
    echo "   If the console is now black, it is holding the adapter and needs"
    echo "   the reset button - a remote reset only works from dcload."
fi

if [ ! -s "$SHOTDIR/$NAME.ppm" ]; then
    echo "!! no capture at $SHOTDIR/$NAME.ppm"
    echo "   The console log is in $LOG. 'popsurf: capture' lines say what the"
    echo "   browser thought it was doing; no such line means it never settled."
    exit 1
fi

# The run has to have been the run that was asked for.
#
# This exists because it once was not: three captures asking for three
# different mask paths all rendered the default, all came back with identical
# numbers, and every one of them read as the experiment succeeding. A silent
# fallback to something that works is the worst possible failure for a
# measurement, so a picture taken in the wrong state is refused here rather
# than compared and believed.
#
# Two checks, because there are two seams. The browser echoes every setting it
# took, which proves the file arrived and was understood; and for a mask path
# it also names the mode that actually drew, which proves the setting reached
# the code that acts on it. The first was fine all along and the second was
# not.
verify_args() {
    local a key val ok=1

    for a in ${ARGS+"${ARGS[@]}"}; do
        key="${a%%=*}"
        val="${a#*=}"

        if ! grep -qF "popsurf: bootargs $key=$val" "$LOG"; then
            echo "!! the console never reported taking '$a'." >&2
            echo "   Either the setting did not reach $MAPDIR/.bootargs or the" >&2
            echo "   browser does not know that key. See $LOG." >&2
            ok=0
            continue
        fi

        if [ "$key" = swfmask ]; then
            if grep -q "MODE NOT APPLIED" "$LOG"; then
                echo "!! the browser read swfmask=$val and then rendered in" >&2
                echo "   another mode. See the MODE NOT APPLIED line in $LOG." >&2
                ok=0
            # The colon matters: the modes are named vol, volswap, volonly and
            # vol1, and without it a run asking for vol is satisfied by a line
            # saying volswap - which is the same class of false pass this whole
            # function exists to stop.
            elif ! grep -qF "popsurf: swf mask $val:" "$LOG"; then
                echo "!! nothing in $LOG says a movie drew in mask mode" >&2
                echo "   '$val'. If the page carries no movie the setting was" >&2
                echo "   never exercised, and this capture is not evidence" >&2
                echo "   about it either way." >&2
                ok=0
            fi
        fi

        # The same rule for the PVR configuration, which is chosen before the
        # video hardware is even up: the browser names the profile it came up
        # with, and a capture taken under another one says nothing about the
        # one that was asked for.
        if [ "$key" = pvrcfg ] &&
           ! grep -qF "popsurf: pvr config $val:" "$LOG"; then
            echo "!! the console did not come up in pvr profile '$val'." >&2
            echo "   See the 'popsurf: pvr config' line in $LOG." >&2
            ok=0
        fi
    done

    # A frame that is going to be compared has to have been drawn without
    # dithering. The hardware answers a colour its five-bit channels cannot
    # hold by alternating the two either side of it, which is right on a
    # television and is noise here: it cost two rounds of chasing "wrong pixels
    # on one scanline at a regular stride" that were the display working
    # correctly. The browser turns it off for any run that is going to be
    # captured and says so; if it did not, this is not a measurement.
    case " ${ARGS+${ARGS[*]}} " in
        *" dither="*) ;;
        *)  if [ "$COMPARE" -eq 1 ] &&
               ! grep -q "popsurf: dither off" "$LOG"; then
                echo "!! the console did not turn dithering off for this" >&2
                echo "   capture, so a comparison against it would count the" >&2
                echo "   display's own colour approximation as faults. A" >&2
                echo "   build from before that landed never says it; pass" >&2
                echo "   --arg dither=1 to compare one anyway." >&2
                ok=0
            fi ;;
    esac

    # And the other half of the display path: KOS renders through a 0.999
    # vertical scale on a television cable, which makes every framebuffer row a
    # blend of two and every horizontal edge a ramp. Same rule, same reason.
    case " ${ARGS+${ARGS[*]}} " in
        *" vsmooth="*) ;;
        *)  if [ "$COMPARE" -eq 1 ] &&
               ! grep -q "popsurf: vsmooth off" "$LOG"; then
                echo "!! the console did not turn vertical smoothing off for" >&2
                echo "   this capture, so every horizontal edge in it is a" >&2
                echo "   two or three row ramp. Pass --arg vsmooth=1 to" >&2
                echo "   compare one anyway." >&2
                ok=0
            fi ;;
    esac

    [ "$ok" -eq 1 ]
}

if ! verify_args; then
    echo "!! refusing to compare: the console did not run what was asked for." >&2
    echo "   The capture is still at $OUTDIR/$NAME.ppm if it is wanted." >&2
    mv "$SHOTDIR/$NAME.ppm" "$OUTDIR/$NAME.ppm" 2>/dev/null || true
    exit 1
fi

mv "$SHOTDIR/$NAME.ppm" "$OUTDIR/$NAME.ppm"
echo ">>> $OUTDIR/$NAME.ppm"
# The header only: the browser writes what it loaded and where the movie
# landed into the PPM's comments, so the file says what it is a picture of.
head -c 400 "$OUTDIR/$NAME.ppm" | sed -n 's/^#/   #/p'

[ "$COMPARE" -eq 1 ] || exit 0

# The movie this page carries, if it carries one. Same basename beside the
# page, which is how the generated testbeds are laid out.
if [ -z "$SWF" ]; then
    CAND="$MAPDIR/${URL%.html}.swf"
    [ -f "$CAND" ] && SWF="$CAND"
fi
if [ -z "$SWF" ]; then
    echo ">>> no movie to compare against; the frame is in $OUTDIR/$NAME.ppm"
    exit 0
fi

# What the page paints behind the stage. The player draws no background of its
# own - the stage is transparent and the page shows through, which is what a
# plugin does - while swfrender fills its canvas with the file's declared
# background, so the two disagree over every pixel the movie did not draw
# unless the substitution is told what to use. Taken from the page rather than
# assumed, and printed, because a wrong colour here would read as the renderer
# painting the wrong background.
if [ -z "$BG" ]; then
    BG="$(grep -o 'bgcolor="#[0-9a-fA-F]\{6\}"' "$MAPDIR/${URL}" 2>/dev/null \
          | tail -1 | grep -o '[0-9a-fA-F]\{6\}' || true)"
fi

echo ">>> reference: $SWF  frame $FRAME"
make -C tests/swf-host swfrender ppmcmp > /dev/null
tests/swf-host/swfrender "$SWF" -o "$OUTDIR/$NAME-ref" -s 1 -f "$FRAME" \
    | sed -n 's/^/   /p' | head -4

REF="$(printf '%s/%s-ref-f%03d.ppm' "$OUTDIR" "$NAME" "$FRAME")"
CMP=(tests/swf-host/ppmcmp "$OUTDIR/$NAME.ppm" "$REF"
     -o "$OUTDIR/$NAME-diff.ppm")
[ -n "$BG" ] && CMP+=(-b "$BG")
set +e
"${CMP[@]}"
CMPRC=$?
set -e

# Where the disagreements are, not only how many. A count of fifteen is a
# hairline crack, a missing sliver or a wrong corner depending on where the
# fifteen sit, and those are different faults; the map is already written, so
# reading it back costs nothing and settles which.
if [ -x tools/diffwhere.py ] && command -v python3 > /dev/null; then
    tools/diffwhere.py "$OUTDIR/$NAME-diff.ppm" || true
fi
exit "$CMPRC"
