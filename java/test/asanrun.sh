#!/bin/sh
# Runs every applet in the tree under AddressSanitizer and UBSan.
#
# The container classes hand the collector references it must trace and hold
# objects across allocations that can collect, and both mistakes look like
# nothing at all until an applet has been running for a minute on hardware
# with no memory protection. A host with a sanitizer finds them in a second.
set -e

# jrun does not free its font or its cache on the path where an applet has no
# paint method, and two of the classes here are helpers rather than applets.
# Leaks at exit are not what this is looking for.
ASAN_OPTIONS=detect_leaks=0
export ASAN_OPTIONS
cd "$(dirname "$0")"

# Every runtime source except the two that only exist on a console:
# ps_jtest.c needs kos.h and ps_jdc.c binds the Dreamcast glyph cache.
# Globbed rather than listed so a new ps_*.c does not silently miss.
gcc -g -O1 -fsanitize=address,undefined \
    -I.. -I../../core -I../../gfx -I../../net -I../../vendor \
    jrun.c $(ls ../ps_*.c | grep -Ev "ps_(jtest|jdc)\.c") -lm -o jrun_asan

mkdir -p out
fail=0

for f in applets/*.class; do
    t=$(basename "$f" .class)
    if ./jrun_asan "$f" "" "out/asan_$t.ppm" 3 > "out/asan_$t.log" 2>&1; then
        echo "ok    $t"
    else
        if grep -q "applet did not run" "out/asan_$t.log"; then
            echo "skip  $t  (a helper class, not an applet)"
            continue
        fi
        echo "FAIL  $t"
        grep -m1 -A 12 'ERROR\|runtime error' "out/asan_$t.log" || true
        fail=1
    fi
done

exit $fail
