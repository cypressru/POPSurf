/* The interior/boundary comparison. See ps_ppmdiff.h for the rule. */
#include "ps_ppmdiff.h"

#include <string.h>

void ps_ppmdiff(const uint8_t *ref, const uint8_t *cmp, int w, int h,
                int tol, uint8_t *map, ps_ppmdiff_result *out)
{
    long k, npx = (long)w * h;

    memset(out, 0, sizeof *out);
    out->counted = npx;
    if(map)
        memset(map, PS_PPMDIFF_SAME, (size_t)npx);

    for(k = 0; k < npx; k++) {
        const uint8_t *pa = ref + k * 3, *pb = cmp + k * 3;
        int x = (int)(k % w), y = (int)(k / w);
        int d = 0, c, inner = 1, dx, dy;

        for(c = 0; c < 3; c++) {
            int e = pa[c] - pb[c];

            if(e < 0) e = -e;
            if(e > d) d = e;
        }
        if(d > out->worst) {
            out->worst   = d;
            out->worst_x = x;
            out->worst_y = y;
        }
        if(d <= tol)
            continue;
        if(d > tol + 1)
            out->worse++;

        if(x == 0 || y == 0 || x == w - 1 || y == h - 1) {
            inner = 0;
        }
        else {
            for(dy = -1; dy <= 1 && inner; dy++)
                for(dx = -1; dx <= 1; dx++) {
                    const uint8_t *q = pa + ((long)dy * w + dx) * 3;

                    if(q[0] != pa[0] || q[1] != pa[1] || q[2] != pa[2]) {
                        inner = 0;
                        break;
                    }
                }
        }

        if(inner) out->interior++;
        else      out->boundary++;
        if(map)
            map[k] = inner ? PS_PPMDIFF_INTERIOR : PS_PPMDIFF_BOUNDARY;
    }
}
