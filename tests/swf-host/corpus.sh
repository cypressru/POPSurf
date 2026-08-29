#!/usr/bin/env bash
# Run the parser, the renderer and the interpreter over real SWF files that
# nobody here wrote.
#
# Everything in `make check` is synthetic. mkswf writes the file and ps_swf
# reads it, and while every expected number is arithmetic rather than a
# recorded output - which is what makes that suite worth having - a
# misunderstanding of the format shared by both sides cancels out and passes.
# Until this file existed, no subsystem here but the shape parser had seen a
# byte emitted by Adobe's tools, or by Ming, or by anything else.
#
# It is worth running after any decoder lands, and worth reading rather than
# only passing. When morph shapes arrived, Ming's morph_test1 went from drawing
# nothing on all eight of its frames to drawing twenty-two characters, and that
# one line is the only independent evidence in the tree that the morph work is
# right - every other test of it was written by the hand that wrote the reader.
#
# The corpus is Ruffle's regression suite (github.com/ruffle-rs/ruffle,
# MIT OR Apache-2.0). Two things make it the right one: the files are small and
# purpose-built, so a failure points at a construct rather than at a movie; and
# most of them ship an output.txt holding the trace a real player produces,
# which turns a file from input into an oracle.
#
# Nothing is committed. The files are fetched to a temporary directory at run
# time and are not copied into the tree - their provenance is not ours to
# redistribute, and that applies to a render of one as much as to the file.
#
#   ./corpus.sh              fetch, run, summarise
#   ./corpus.sh -k           keep the fetched files and the renders
#   SWF_CORPUS_DIR=... ./corpus.sh
#
# Deliberately not part of `make check`: check has to run wherever the
# repository does, and this needs the network. With no network it says so and
# exits 0, so wiring it into a build that sometimes has one is safe.
#
# --- compression -------------------------------------------------------------
#
# Most of this corpus is CWS, and ps_swf_load refuses CWS - correctly, at the
# stated target, since compression arrived in SWF 6. Left there, nine of the
# files below would be one line of refusal each and the renderer would get no
# real-content coverage at all, which is the opposite of the point.
#
# So when python3 is present a CWS file is inflated here, on the host, and the
# parser is handed the FWS equivalent. That tests what can be tested - the tag
# stream, the shapes, the display list, the masks - without pretending the
# player can read the file, and the row says which files went through it. It is
# a measuring instrument and not a workaround: nothing in the player changes,
# and the refusal that a real page would get is still what the player does.
#
# --- how to read the result --------------------------------------------------
#
# parsed / refused / crashed is the top-level answer and only the last is a
# defect by itself. A refusal with a reason is the parser doing its job; a
# refusal whose reason is a tag we should have handled is a gap, and the reason
# is printed so the two can be told apart. A crash is always ours.
#
# A file that parses can carry a note too, and the useful one names a tag this
# build declines. "drew 0; DefineShape4 is not read" is a complete answer;
# "drew 0" on its own is a question, and telling those apart on the row rather
# than by opening the file is most of what this column is worth.
#
# The trace comparison is subtler, and a mismatch is three different things:
#
#   - an action we implement and get wrong. This is the one worth fixing and
#     the summary line's skipped=0 is what identifies it.
#   - an action we do not implement, which the VM skips. skipped counts them
#     and first_skipped names the first opcode.
#   - a control flow swftrace does not model - it makes one pass and has no
#     playhead, so anything that gotos, loops a sprite, or leans on a frame
#     running twice will diverge no matter how right the interpreter is.
#
# So a mismatch here is a question, not a failure, and the script does not
# pretend otherwise: it exits non-zero only for a crash.
set -u
cd "$(dirname "$0")"

# Overridable so the no-network path can be exercised deliberately - point it
# at a host that does not resolve and the script has to say one line and exit 0.
BASE=${SWF_CORPUS_BASE:-https://raw.githubusercontent.com/ruffle-rs/ruffle/master/tests/tests/swfs}
DIR=${SWF_CORPUS_DIR:-/tmp/ps-swf-corpus}
KEEP=0
[ "${1:-}" = "-k" ] && KEEP=1

# Chosen to hit the subsystems that have only ever seen synthetic input, and
# nothing else. The avm1 group is the SWF 4 action set, which is the whole of
# what ps_swf_action.c claims; simple_shapes is the renderer's own list of
# things it is easy to get wrong, written by people who had to fix each one;
# and misc-ming.all is Ming's output, which is a third encoder again and the
# only source here of a morph, an EditText and a Flash 4 era button.
FILES="
avm1/add_swf4
avm1/divide_swf4
avm1/equals_swf4
avm1/equals_swf4_alt
avm1/lessthan_swf4
avm1/lessthan_swf4_alt
avm1/logical_ops_swf4
avm1/swf4_bool
avm1/swf4_vars
avm1/swf4_actions_bool
avm1/swf4_actions_coercion_order
avm1/action_to_integer
avm1/getproperty_swf4
avm1/tell_target
avm1/waitforframe
avm1/string_relational_compare
visual/simple_shapes/masks
visual/simple_shapes/masks_equal_clipdepth
visual/simple_shapes/winding_rule
visual/simple_shapes/layers
visual/simple_shapes/overlaps
visual/simple_shapes/text_field_mask
visual/simple_shapes/gradients/gradients
visual/simple_shapes/gradients/radial
visual/simple_shapes/gradients/repeat
visual/simple_shapes/strokes/scale
visual/color_transform_issue_9698
visual/gradient_nonsequential_ratios
visual/gradient_same_ratios
from_gnash/misc-ming.all/morph_test1
from_gnash/misc-ming.all/masks_test
from_gnash/misc-ming.all/DefineTextTest
from_gnash/misc-ming.all/DefineEditTextTest
from_gnash/misc-ming.all/GradientFillTest
from_gnash/misc-ming.all/ButtonEventsTest
from_gnash/misc-ming.all/PlaceObject2Test
from_gnash/misc-ming.all/matrix_test
from_gnash/misc-ming.all/displaylist_depths/displaylist_depths_test
from_gnash/misc-ming.all/place_object_test
"

make -s swfrender swftrace ppmcheck || exit 1

mkdir -p "$DIR" || exit 1

# One fetch decides whether there is a network, before anything is reported, so
# a machine with no route says one line rather than forty timeouts.
if ! curl -s -f -m 15 -o "$DIR/.probe" "$BASE/avm1/add_swf4/test.swf" \
        2>/dev/null; then
    echo "corpus: no network, or the upstream layout moved - skipping."
    echo "corpus: nothing here is required for 'make check'."
    exit 0
fi

parsed=0; refused=0; crashed=0; matched=0; differed=0; notrace=0
inflated=0; known=0; blank=0; cws=0
faults=""

# Inflate a CWS in place, on the host, into the FWS the parser can walk. The
# eight-byte header is not compressed - signature, version and the uncompressed
# length - so only what follows it goes through zlib.
inflate() {
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import sys, zlib
d = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'wb').write(b'FWS' + d[3:8] + zlib.decompress(d[8:]))
PY
}

printf '%-45s %-10s %s\n' FILE RESULT NOTE
printf -- '----------------------------------------------------------------------\n'

for p in $FILES; do
    n=$(echo "$p" | tr / _)
    swf="$DIR/$n.swf"
    exp="$DIR/$n.expected"
    tag=""

    if [ ! -s "$swf" ]; then
        curl -sS -f -m 20 -o "$swf" "$BASE/$p/test.swf" 2>/dev/null || {
            printf '%-45s %-10s %s\n' "$p" "absent" "no test.swf upstream"
            rm -f "$swf"
            continue
        }
    fi
    [ -s "$exp" ] || curl -sS -f -m 20 -o "$exp" "$BASE/$p/output.txt" \
        2>/dev/null || rm -f "$exp"

    # Ruffle marks its own known-bad expectations. An output.txt behind one of
    # those is not an oracle, so a mismatch against it says nothing about us.
    if [ ! -s "$DIR/$n.toml" ]; then
        curl -sS -f -m 20 -o "$DIR/$n.toml" "$BASE/$p/test.toml" 2>/dev/null ||
            : > "$DIR/$n.toml"
    fi
    if grep -q '^known_failure *= *true' "$DIR/$n.toml" 2>/dev/null; then
        known=$((known + 1))
        tag="[ruffle known_failure] "
        rm -f "$exp"
    fi

    run="$swf"
    if [ "$(head -c 3 "$swf")" = "CWS" ]; then
        cws=$((cws + 1))
        if inflate "$swf" "$DIR/$n.fws" && [ -s "$DIR/$n.fws" ]; then
            run="$DIR/$n.fws"
            inflated=$((inflated + 1))
            tag="${tag}[CWS, inflated here] "
        fi
    fi

    # The render is what exercises the parser, the display list and both
    # rasterisers on a real file. One frame, because the question is whether it
    # survives the file at all and a hundred frames of a movie answers that no
    # better than one does - the last frame rather than the first, since a file
    # that places nothing until frame 1 is common and its frame 0 is honestly
    # empty. -f is clamped to what the file has, so the number only has to be
    # larger than any frame count in the corpus.
    out=$(timeout 25 ./swfrender "$run" -o "$DIR/$n" -f 99999 -s 1 \
          2>"$DIR/$n.err")
    rc=$?
    err=$(head -1 "$DIR/$n.err")
    ppm=$(printf '%s\n' "$out" | sed -n 's/.*-> //p' | head -1)

    if [ $rc -ge 124 ]; then
        crashed=$((crashed + 1))
        faults="$faults\n  $p: swfrender exit $rc (signal or 25s timeout)"
        printf '%-45s %-10s %s\n' "$p" "CRASHED" "exit $rc"
        continue
    fi
    if [ $rc -ne 0 ]; then
        refused=$((refused + 1))
        printf '%-45s %-10s %s\n' "$p" "refused" "$tag${err#*: }"
        continue
    fi
    parsed=$((parsed + 1))

    # Whatever the walk stopped on, if it stopped early, plus how much of the
    # file turned into something drawable. A real file that parses cleanly and
    # then draws nothing is the failure this corpus exists to find, and it is
    # invisible in an exit status.
    note=$(printf '%s\n' "$out" | sed -n 's/^note: //p' | head -1)
    drew=$(printf '%s\n' "$out" | sed -n 's/.*: *\([0-9]*\) characters.*/\1/p' |
           head -1)
    note="drew ${drew:-0}${note:+; $note}"

    # Whether anything reached the canvas, which is not the same question as
    # how many characters the walker handed to the renderer and is the one that
    # matters. A frame that is uniformly the background colour is a blank page,
    # however many characters were drawn into it. The two simple_shapes/masks
    # files were exactly that until a mask over an unresolvable character
    # stopped being built as an empty one: four characters drawn, nothing
    # visible, and a clean exit status saying so.
    if [ -s "$ppm" ] && [ "${drew:-0}" != "0" ]; then
        bg=$(printf '%s\n' "$out" | sed -n 's/.*bg \([0-9a-f]*\).*/\1/p' | head -1)
        set -- $(sed -n '2p' "$ppm")
        if ./ppmcheck "$ppm" "${bg:-ffffff}" $(( ${1:-0} * ${2:-0} )) \
               >/dev/null 2>&1; then
            note="$note, but the frame is all background"
            blank=$((blank + 1))
        fi
    elif [ "${drew:-0}" = "0" ] &&
         ! printf '%s\n' "$out" | grep -q 'root [0-9]* frames, 0 ops'; then
        # A file with no placements is a script test and has nothing to show.
        # One with placements that drew nothing lost its characters somewhere.
        blank=$((blank + 1))
        note="$note, though the root places things"
    fi

    if [ ! -s "$exp" ]; then
        notrace=$((notrace + 1))
        printf '%-45s %-10s %s\n' "$p" "parsed" "$tag$note"
        continue
    fi

    got=$(timeout 25 ./swftrace "$run" 2>"$DIR/$n.diag")
    trc=$?
    sum=$(sed -n 's/^summary parsed //p' "$DIR/$n.diag" | head -1 |
          sed 's/ note=-//; s/first_skipped=-1/first=none/; s/ status=-//;
               s/status=action whose body cannot be skipped/STOPPED/')
    if [ $trc -ge 124 ]; then
        crashed=$((crashed + 1))
        faults="$faults\n  $p: swftrace exit $trc"
        printf '%-45s %-10s %s\n' "$p" "CRASHED" "swftrace exit $trc"
        continue
    fi

    if [ "$got" = "$(cat "$exp")" ]; then
        matched=$((matched + 1))
        printf '%-45s %-10s %s\n' "$p" "trace ok" \
               "$tag$(printf '%s\n' "$got" | wc -l) lines agree"
    else
        differed=$((differed + 1))
        first=$(diff <(printf '%s\n' "$got") "$exp" |
                sed -n '2,3p' | tr '\n' ' ' | cut -c1-46)
        printf '%-45s %-10s %s\n' "$p" "trace ne" "$tag$sum | $first"
    fi
done

printf -- '----------------------------------------------------------------------\n'
echo "parsed $parsed   refused $refused   crashed $crashed"
echo "of the parsed: trace matched $matched, differed $differed," \
     "no usable oracle $notrace"
# Expected for a script test, which places a text field and never draws it, and
# a defect for anything under visual/. The per-row note says which is which.
echo "$blank placed something and then showed nothing"
echo "$cws are CWS and so refused outright by the player itself;" \
     "$inflated of those were inflated here to test what is behind them"
echo "$known carry Ruffle's own known_failure and so are not oracles"
[ -n "$faults" ] && printf 'crashes:%b\n' "$faults"

[ $KEEP -eq 1 ] || rm -f "$DIR"/*.ppm "$DIR"/.probe

exit $((crashed > 0 ? 1 : 0))
