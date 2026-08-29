#!/bin/sh
# Runs the java.lang / java.util test applets twice - once on a desktop JVM,
# once on this browser's runtime - and diffs the two.
#
# That comparison is the whole point. There is no JVM specification in the
# tree, so every edge these classes have (what substring refuses, where
# lastIndexOf counts from, which sequence a seeded Random produces) is settled
# by asking a real implementation and matching it. A test that only checked
# the output against itself would pass on any consistent set of wrong answers.
#
#   cd java/test && ./libcheck.sh
#
# javac here must emit a class file this parser accepts, which means
# --release 8: the interpreter takes versions 45 through 52, and a modern
# javac defaults well past that.
set -e

cd "$(dirname "$0")"

TESTS="LibString LibNum LibColl LibRand LibMisc LibStress LibTwo Poly"

echo "building jrun"
# Every runtime source except the two that only exist on a console:
# ps_jtest.c needs kos.h and ps_jdc.c binds the Dreamcast glyph cache.
# Globbed rather than listed so a new ps_*.c does not silently miss.
gcc -O2 -I.. -I../../core -I../../gfx -I../../net -I../../vendor \
    jrun.c $(ls ../ps_*.c | grep -Ev "ps_(jtest|jdc)\.c") -lm -o jrun

# -sourcepath so a test that leans on a second class finds it, which is also
# how the multi-class resolution path gets exercised at all.
echo "compiling applets"
for t in $TESTS; do
    javac --release 8 -nowarn -sourcepath applets -d applets "applets/$t.java"
done

mkdir -p ref out
fail=0

for t in $TESTS; do
    # The reference is regenerated when a JVM is to hand and reused when it is
    # not, so the check still runs on a machine with no JDK installed.
    if command -v java >/dev/null 2>&1; then
        java -cp applets "$t" > "ref/$t.txt"
    fi

    # The runtime prints an applet's System.out through the browser log, one
    # line per println, prefixed. Everything else jrun says is about jrun.
    ./jrun "applets/$t.class" "" "out/$t.ppm" 0 2>&1 \
        | sed -n 's/^applet: //p' > "out/$t.txt"

    if diff -a -u "ref/$t.txt" "out/$t.txt" > "out/$t.diff"; then
        echo "ok    $t  ($(wc -l < "ref/$t.txt") lines)"
    else
        echo "FAIL  $t"
        cat "out/$t.diff"
        fail=1
    fi
done

exit $fail
