#!/usr/bin/env bash
# Lay out a set of real pages both ways and report what the fit-to-viewport
# policy changed.
#
# The policy is a deliberate departure from CSS - see Docs/layout-policy.md -
# and the only thing that makes such a departure defensible is being able to
# show, on real pages, that it helps where it should and does nothing where it
# should not. Run this after touching anything in layout.
#
# What to look for:
#   - A page that already fitted must report "no difference". If one starts
#     changing, the policy has grown past what it was meant to do.
#   - A page that overflowed should report a smaller fitted edge. If it still
#     overflows, something is demanding width by a route the policy misses -
#     that is how the fixed-width table case was found after the min-width
#     case was already handled.
#
# These are live URLs, so this needs a network and the results can change
# under you when a site is edited. That is the point: they are the pages this
# browser exists to read.
set -u
cd "$(dirname "$0")"

[ -x ./layouttest ] || make -s

PAGES="
bb.dreampipe.net/
dreampipe.net/
sonic2.dreampipe.net/
dreampipe.net/Games/sfa3/index.html
dreampipe.net/Games/CrazyTaxi2/home.html
dc99.net/Games/../dc/index2.html
mania.dc99.net/
"

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

for u in $PAGES; do
    if ! curl -s -m 15 "http://$u" -o "$TMP" 2>/dev/null; then
        printf '%-44s unreachable\n' "$u"
        continue
    fi
    printf '%-44s ' "$u"
    ./layouttest "$TMP" "${1:-640}" --compare 2>&1 | tail -3 |
        tr '\n' ' ' | sed 's/  */ /g'
    echo
done

echo
echo "Local pages:"
for f in cell-height.html table-regress.html ../../cd/sites.html \
         ../../cd/test_cells.html; do
    [ -f "$f" ] || continue
    printf '%-44s ' "$(basename "$f")"
    ./layouttest "$f" "${1:-640}" --compare 2>&1 | tail -3 |
        tr '\n' ' ' | sed 's/  */ /g'
    echo
done
