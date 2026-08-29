#!/usr/bin/env bash
# Drives swffuzz under the sanitizers and keeps the register of what is still
# open.
#
# -fno-sanitize-recover=all means the first fault ends the process, which is
# the only setting under which a green run means anything - a sanitizer that
# reports and carries on lets a run finish clean with a page of reports in the
# scrollback. The cost is that one known defect stops the sweep at the case
# that hits it and the remaining hundred thousand never run.
#
# So this resumes. When a run dies, the report is matched against the register
# below. A match is announced, counted, and the sweep restarts at the next
# case; anything else is printed in full and fails the target. The register is
# therefore the whole of what "clean" is being claimed relative to, it is in
# the repository rather than in somebody's head, and it is printed on every
# run - a finding that stops being mentioned is a finding that has been fixed
# or a register that has been edited, and both are visible in a diff.
#
# GCC's libubsan ignores UBSAN_OPTIONS=suppressions, so this cannot be done
# with a suppression file; it was tried first.
#
#   ./fuzz.sh ./swffuzz-san 1 20260807
#
# The seeds are arguments and not a default, because `make fuzz` states them
# and a run has to be repeatable from the command that produced it.

set -u
cd "$(dirname "$0")" || exit 1

BIN=${1:?usage: fuzz.sh <swffuzz binary> <seed>...}
shift
[ $# -gt 0 ] || { echo "fuzz.sh: no seeds given" >&2; exit 2; }

# Case counts per seed. Deliberately not tuned per machine: a run has to be the
# same run everywhere or the numbers in a report mean nothing.
MUTANTS=3000
GARBAGE=20000

export ASAN_OPTIONS=abort_on_error=1:detect_leaks=1
# The stack trace is what lets the register key on a function rather than on a
# line number, which moves whenever the file above it is edited.
export UBSAN_OPTIONS=abort_on_error=1:print_stacktrace=1

# --- the register --------------------------------------------------------
#
# One entry per line: kind | frame or file | what it is.
#
# Both entries below are signed overflow and neither is a bound, which is why
# they are here rather than fixed at the first sighting - see the report that
# came with them. Nothing else is expected, and a third line appearing here
# without an argument beside it would be this file doing the opposite of its
# job.
KNOWN=(
"signed integer overflow|parse_define_text|DefineText accumulates each glyph's advance into an int; a file stating an advance near INT32_MAX overflows it. swf/ps_swf_parse.c, and ours to fix."
"signed integer overflow|stb_image.h|stb_image's JPEG IDCT is written throughout for wrapping int arithmetic and overflows on adversarial coefficients. Vendored third-party code; any other check failing in that file is not covered by this line."
)

echo "--- open findings this run is allowed to hit"
for k in "${KNOWN[@]}"; do
    printf '  %s\n' "${k##*|}"
done

log=$(mktemp)
trap 'rm -f "$log" "$log.err"' EXIT

# The size of the case space, which is what a seed's sweep covers less the
# handful of cases that die. Asked for rather than added up from the segments:
# a segment that dies reports nothing, so summing what the segments printed
# undercounts by however much of the space came before the fault.
SPACE=$("$BIN" -s 1 -n $MUTANTS -g $GARBAGE -N out/t_*.swf)

total=0
hits=0
newfail=0

for seed in "$@"; do
    at=0
    while :; do
        # In a subshell, and with something after the command so bash does not
        # replace the subshell with it: the shell's own "Aborted" notice is
        # then written by the subshell, lands in the log beside the report it
        # belongs to, and stays out of the middle of this script's output. A
        # run that stops on a known finding is expected here, so the notice is
        # noise rather than news.
        ( "$BIN" -s "$seed" -n $MUTANTS -g $GARBAGE -b "$at" out/t_*.swf; \
          exit $? ) > "$log" 2> "$log.err"
        rc=$?

        grep -v 'swffuzz: [0-9]* cases in\|swffuzz: clean\|swffuzz: .* seeds,' \
             "$log"

        if [ $rc -eq 0 ]; then
            total=$((total + SPACE))
            break
        fi

        case_at=$(sed -n 's/.*last case: case \([0-9]*\):.*/\1/p' "$log.err" \
                  | head -1)
        desc=$(sed -n 's/.*last case: \(.*\)/\1/p' "$log.err" | head -1)

        matched=""
        for k in "${KNOWN[@]}"; do
            kind=${k%%|*}; rest=${k#*|}; where=${rest%%|*}
            if grep -q "$kind" "$log.err" && grep -q "$where" "$log.err"; then
                matched=$where
                break
            fi
        done

        if [ -z "$matched" ] || [ -z "$case_at" ]; then
            echo "=== NOT IN THE REGISTER - seed $seed"
            cat "$log.err"
            newfail=1
            break
        fi

        hits=$((hits + 1))
        echo "  known ($matched) at $desc, resuming"
        at=$((case_at + 1))
    done
done

# Every case in the space bar the ones that died on a known finding, which are
# subtracted rather than glossed: they are the only cases in the sweep whose
# whole path did not run.
total=$((total - hits))
echo "--- $total of $((SPACE * $#)) cases over $# seed(s);" \
     "$hits stopped on an open finding"
if [ $newfail -ne 0 ]; then
    echo "FAIL: a report that is not in the register above."
    exit 1
fi
if [ $hits -eq 0 ]; then
    # Worth saying out loud rather than quietly passing: either the corpus
    # stopped reaching them or somebody fixed them, and the register should
    # then lose a line.
    echo "note: no open finding was reached this run."
fi
echo "swffuzz: no report outside the register."
