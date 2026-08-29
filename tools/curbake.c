/* Converts Windows .cur/.ani files into a bounded little-endian .psc bundle. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

/* Sanity ceilings. A cursor bigger than this is either corrupt or something we
 * would refuse to draw anyway, and refusing early keeps the allocations small.
 */
#define CB_MAX_DIM      256
#define CB_MAX_STEPS    512
#define CB_MAX_FILE     (8u * 1024u * 1024u)

#define CB_N_ROLES      19

/* ------------------------------------------------------------------------ */

static unsigned rd_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd_u32(const unsigned char *p)
{
    return (unsigned long)p[0]        | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static long rd_s32(const unsigned char *p)
{
    unsigned long v = rd_u32(p);

    /* Avoid implementation-defined conversion of out-of-range unsigned. */
    return (v & 0x80000000UL) ? -(long)(0xFFFFFFFFUL - v) - 1L : (long)v;
}

/* True when [off, off+need) fits inside a buffer of len bytes, computed so it
 * cannot itself overflow.
 */
static int fits(size_t len, size_t off, size_t need)
{
    return off <= len && need <= len - off;
}

static void put_u16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
}

static void put_u32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
    fputc((int)((v >> 16) & 0xff), f);
    fputc((int)((v >> 24) & 0xff), f);
}

/* ------------------------------------------------------------------------ */

typedef struct {
    int             w, h;
    int             hot_x, hot_y;
    unsigned long   delay_ms;
    unsigned short *px;            /* w*h ARGB4444, host order until written */
} cb_frame;

typedef struct {
    cb_frame *f;
    int       n, cap;
    char      src[512];            /* filename it came from, for the report */
} cb_role;

static int role_push(cb_role *r, const cb_frame *f)
{
    if(r->n == r->cap) {
        int       ncap = r->cap ? r->cap * 2 : 8;
        cb_frame *nf   = (cb_frame *)realloc(r->f, (size_t)ncap * sizeof *nf);

        if(!nf)
            return -1;
        r->f   = nf;
        r->cap = ncap;
    }
    r->f[r->n++] = *f;
    return 0;
}

/* ------------------------------------------------------------------------ */

/* Role ids are the wire format; the runtime indexes the role table with them,
 * so these numbers must never be reordered.
 */
static const char *g_role_name[CB_N_ROLES] = {
    "default", "pointer", "text", "wait", "progress", "help", "crosshair",
    "move", "not-allowed", "ew-resize", "ns-resize", "nesw-resize",
    "nwse-resize", "context-menu", "handwriting", "location", "person",
    "wait-static", "progress-static"
};

typedef struct {
    const char *needle;
    int         role;
} cb_match;

/* Scanned in order, first hit wins. The animated variants come first because
 * "busy (animated).ani" also contains the static role's stem; ordering is the
 * whole disambiguation strategy here.
 */
static const cb_match g_match[] = {
    { "busy (animated)",                  3  },
    { "working in background (animated)", 4  },
    { "normal select",                    0  },
    { "link select",                      1  },
    { "text select",                      2  },
    { "help",                             5  },
    { "precision",                        6  },
    { "move",                             7  },
    { "unavailable",                      8  },
    { "horizontal resize",                9  },
    { "vertical resize",                  10 },
    { "positive diag resize",             11 },
    { "negative diag resize",             12 },
    { "alternative select",               13 },
    { "handwriting",                      14 },
    { "location select",                  15 },
    { "person select",                    16 },
    { "busy (static)",                    17 },
    { "working in background.cur",        18 }
};

#define CB_N_MATCH ((int)(sizeof g_match / sizeof g_match[0]))

/* ------------------------------------------------------------------------ */

static unsigned short to_argb4444(unsigned a, unsigned r, unsigned g, unsigned b)
{
    return (unsigned short)(((a >> 4) << 12) | ((r >> 4) << 8) |
                            ((g >> 4) << 4)  |  (b >> 4));
}

/* Decode one .cur/.ico image into a frame.
 *
 * `what` is only used to make the error messages point at the offending file,
 * which matters when a pack has thirty of these in it.
 */
static int cur_decode(const unsigned char *d, size_t len, const char *what,
                      cb_frame *out)
{
    unsigned      type, count, i, best = 0;
    unsigned long best_area = 0;
    size_t        ent, img_off;
    unsigned long hdr_size, bytes_in_res;
    long          bi_w, bi_h;
    unsigned      bpp;
    unsigned long compression, clr_used, ncol;
    int           w, h, hot_x, hot_y, has_mask, use_alpha;
    size_t        pal_off, xor_off, and_off, xor_stride, and_stride;
    const unsigned char *pal;
    unsigned short      *px;
    int           x, y;

    if(!fits(len, 0, 6)) {
        fprintf(stderr, "curbake: %s: truncated ICONDIR\n", what);
        return -1;
    }
    type  = rd_u16(d + 2);
    count = rd_u16(d + 4);
    if(rd_u16(d) != 0 || (type != 1 && type != 2) || count == 0) {
        fprintf(stderr, "curbake: %s: not a .cur/.ico (type %u, count %u)\n",
                what, type, count);
        return -1;
    }
    if(!fits(len, 6, (size_t)count * 16)) {
        fprintf(stderr, "curbake: %s: truncated directory\n", what);
        return -1;
    }

    /* Multi-size cursors exist; take the biggest image and let the runtime
     * scale down if it ever needs to.
     */
    for(i = 0; i < count; i++) {
        const unsigned char *e = d + 6 + i * 16;
        unsigned long        a = (unsigned long)(e[0] ? e[0] : 256) *
                                 (unsigned long)(e[1] ? e[1] : 256);
        if(a > best_area) {
            best_area = a;
            best      = i;
        }
    }

    ent          = 6 + (size_t)best * 16;
    hot_x        = (type == 2) ? (int)rd_u16(d + ent + 4) : 0;
    hot_y        = (type == 2) ? (int)rd_u16(d + ent + 6) : 0;
    bytes_in_res = rd_u32(d + ent + 8);
    img_off      = (size_t)rd_u32(d + ent + 12);
    (void)bytes_in_res;

    if(!fits(len, img_off, 40)) {
        fprintf(stderr, "curbake: %s: image offset out of range\n", what);
        return -1;
    }
    if(d[img_off] == 0x89 && fits(len, img_off, 8) &&
       memcmp(d + img_off, "\x89PNG\r\n\x1a\n", 8) == 0) {
        fprintf(stderr, "curbake: %s: PNG-compressed cursor not supported\n",
                what);
        return -1;
    }

    hdr_size    = rd_u32(d + img_off);
    bi_w        = rd_s32(d + img_off + 4);
    bi_h        = rd_s32(d + img_off + 8);
    bpp         = rd_u16(d + img_off + 14);
    compression = rd_u32(d + img_off + 16);
    clr_used    = rd_u32(d + img_off + 32);

    if(hdr_size < 40 || hdr_size > 4096) {
        fprintf(stderr, "curbake: %s: unexpected DIB header size %lu\n",
                what, hdr_size);
        return -1;
    }
    if(compression != 0) {
        fprintf(stderr, "curbake: %s: compressed DIB (biCompression %lu) "
                "not supported\n", what, compression);
        return -1;
    }
    if(bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) {
        fprintf(stderr, "curbake: %s: unsupported %u bpp\n", what, bpp);
        return -1;
    }
    /* Negative biHeight (top-down) never occurs in icon resources and the mask
     * pairing would be ambiguous, so reject it instead of guessing.
     */
    if(bi_w <= 0 || bi_h <= 0 || bi_w > CB_MAX_DIM || bi_h > 2 * CB_MAX_DIM) {
        fprintf(stderr, "curbake: %s: bad DIB size %ldx%ld\n", what, bi_w, bi_h);
        return -1;
    }

    /* biHeight covers the XOR bitmap plus the AND mask stacked on top of it, so
     * the real height is half - except for the odd resource that ships no mask.
     */
    if((bi_h & 1) == 0) {
        h        = (int)(bi_h / 2);
        has_mask = 1;
    } else {
        h        = (int)bi_h;
        has_mask = 0;
    }
    w = (int)bi_w;
    if(w <= 0 || h <= 0) {
        fprintf(stderr, "curbake: %s: degenerate size after mask split\n", what);
        return -1;
    }

    ncol = clr_used ? clr_used : (bpp <= 8 ? (1UL << bpp) : 0UL);
    if(ncol > 256) {
        fprintf(stderr, "curbake: %s: palette of %lu entries\n", what, ncol);
        return -1;
    }

    pal_off    = img_off + (size_t)hdr_size;
    xor_stride = (((size_t)w * bpp + 31) / 32) * 4;
    and_stride = (((size_t)w + 31) / 32) * 4;

    if(!fits(len, pal_off, (size_t)ncol * 4)) {
        fprintf(stderr, "curbake: %s: palette out of range\n", what);
        return -1;
    }
    pal     = d + pal_off;
    xor_off = pal_off + (size_t)ncol * 4;
    if(!fits(len, xor_off, xor_stride * (size_t)h)) {
        fprintf(stderr, "curbake: %s: colour bitmap out of range\n", what);
        return -1;
    }
    and_off = xor_off + xor_stride * (size_t)h;
    if(has_mask && !fits(len, and_off, and_stride * (size_t)h))
        has_mask = 0;   /* mask claimed by biHeight but not actually present */

    /* Plenty of 32bpp cursors carry an all-zero alpha channel and rely on the
     * AND mask instead; using their alpha verbatim would blank the cursor.
     */
    use_alpha = 0;
    if(bpp == 32) {
        for(y = 0; y < h && !use_alpha; y++) {
            const unsigned char *r = d + xor_off + (size_t)y * xor_stride;
            for(x = 0; x < w; x++) {
                if(r[x * 4 + 3]) {
                    use_alpha = 1;
                    break;
                }
            }
        }
    }

    px = (unsigned short *)malloc((size_t)w * (size_t)h * sizeof *px);
    if(!px) {
        fprintf(stderr, "curbake: %s: out of memory\n", what);
        return -1;
    }

    for(y = 0; y < h; y++) {
        /* Icon rows are stored bottom-up. */
        const unsigned char *xrow = d + xor_off + (size_t)(h - 1 - y) * xor_stride;
        const unsigned char *mrow = has_mask
                                  ? d + and_off + (size_t)(h - 1 - y) * and_stride
                                  : NULL;

        for(x = 0; x < w; x++) {
            unsigned raw = 0, r = 0, g = 0, b = 0, a = 255, m, nonzero;

            switch(bpp) {
            case 1:
                raw = (xrow[x >> 3] >> (7 - (x & 7))) & 1;
                break;
            case 4:
                raw = (x & 1) ? (xrow[x >> 1] & 0x0f) : (xrow[x >> 1] >> 4);
                break;
            case 8:
                raw = xrow[x];
                break;
            case 24:
                b = xrow[x * 3 + 0];
                g = xrow[x * 3 + 1];
                r = xrow[x * 3 + 2];
                break;
            default: /* 32 */
                b = xrow[x * 4 + 0];
                g = xrow[x * 4 + 1];
                r = xrow[x * 4 + 2];
                a = xrow[x * 4 + 3];
                break;
            }

            if(bpp <= 8) {
                if(raw >= ncol)
                    raw = 0;
                b = pal[raw * 4 + 0];
                g = pal[raw * 4 + 1];
                r = pal[raw * 4 + 2];
            }

            nonzero = (bpp <= 8) ? (raw != 0) : ((r | g | b) != 0);
            m       = mrow ? ((mrow[x >> 3] >> (7 - (x & 7))) & 1) : 0;

            if(bpp == 32 && use_alpha) {
                /* Alpha channel is authoritative; mask is decoration. */
            } else if(m) {
                if(nonzero) {
                    /* AND=1 XOR=1 means "invert the screen". Nothing on the PVR
                     * path reads the framebuffer back, so approximate with
                     * opaque white - these pixels are cursor outlines.
                     */
                    r = g = b = 255;
                    a = 255;
                } else {
                    a = 0;
                }
            } else {
                a = 255;
            }

            px[(size_t)y * w + x] = to_argb4444(a, r, g, b);
        }
    }

    /* A hotspot outside the image is meaningless and would make the runtime
     * draw off by a wild offset; pin it back inside.
     */
    if(hot_x < 0 || hot_x >= w)
        hot_x = 0;
    if(hot_y < 0 || hot_y >= h)
        hot_y = 0;

    out->w        = w;
    out->h        = h;
    out->hot_x    = hot_x;
    out->hot_y    = hot_y;
    out->delay_ms = 0;
    out->px       = px;
    return 0;
}

/* ------------------------------------------------------------------------ */

/* Jiffies (1/60 s) to milliseconds. A 0-jiffy frame means "as fast as the
 * system can go", which on a 60Hz TV output is a strobe; treat anything
 * implausibly short as a normal 100ms beat.
 */
static unsigned long jiffies_to_ms(unsigned long jif)
{
    unsigned long ms = jif * 1000UL / 60UL;

    return (ms < 20UL) ? 100UL : ms;
}

static int ani_decode(const unsigned char *d, size_t len, const char *what,
                      cb_role *role)
{
    size_t        p, body_end;
    unsigned long riff_size;
    unsigned long n_frames = 0, n_steps = 0, disp_rate = 0;
    const unsigned char *rate = NULL, *seq = NULL;
    unsigned long        n_rate = 0, n_seq = 0;
    const unsigned char *icon[CB_MAX_STEPS];
    size_t               icon_len[CB_MAX_STEPS];
    unsigned long        n_icon = 0, s;

    if(!fits(len, 0, 12) || memcmp(d, "RIFF", 4) != 0 ||
       memcmp(d + 8, "ACON", 4) != 0) {
        fprintf(stderr, "curbake: %s: not a RIFF/ACON file\n", what);
        return -1;
    }
    riff_size = rd_u32(d + 4);
    body_end  = 8 + (size_t)riff_size;
    if(body_end > len)
        body_end = len;   /* trust the file length over the header */

    p = 12;
    while(p + 8 <= body_end) {
        const unsigned char *id   = d + p;
        unsigned long        size = rd_u32(d + p + 4);
        size_t               data = p + 8;

        if(!fits(body_end, data, (size_t)size)) {
            fprintf(stderr, "curbake: %s: chunk '%.4s' runs past end\n",
                    what, (const char *)id);
            return -1;
        }

        if(memcmp(id, "anih", 4) == 0 && size >= 36) {
            n_frames  = rd_u32(d + data + 4);
            n_steps   = rd_u32(d + data + 8);
            disp_rate = rd_u32(d + data + 28);
        } else if(memcmp(id, "rate", 4) == 0) {
            rate   = d + data;
            n_rate = size / 4;
        } else if(memcmp(id, "seq ", 4) == 0) {
            seq   = d + data;
            n_seq = size / 4;
        } else if(memcmp(id, "LIST", 4) == 0 && size >= 4 &&
                  memcmp(d + data, "fram", 4) == 0) {
            size_t q   = data + 4;
            size_t end = data + (size_t)size;

            while(q + 8 <= end) {
                unsigned long isz = rd_u32(d + q + 4);

                if(!fits(end, q + 8, (size_t)isz)) {
                    fprintf(stderr, "curbake: %s: icon chunk overruns LIST\n",
                            what);
                    return -1;
                }
                if(memcmp(d + q, "icon", 4) == 0) {
                    if(n_icon >= CB_MAX_STEPS) {
                        fprintf(stderr, "curbake: %s: too many frames\n", what);
                        return -1;
                    }
                    icon[n_icon]     = d + q + 8;
                    icon_len[n_icon] = (size_t)isz;
                    n_icon++;
                }
                /* RIFF pads every chunk to an even length; skipping the pad
                 * byte is what keeps the walk in sync on odd-sized icons.
                 */
                q += 8 + (size_t)isz + (isz & 1);
            }
        }

        p = data + (size_t)size + (size & 1);
    }

    if(n_icon == 0) {
        fprintf(stderr, "curbake: %s: no icon frames found\n", what);
        return -1;
    }
    if(n_frames != n_icon)
        fprintf(stderr, "curbake: %s: anih claims %lu frames, found %lu\n",
                what, n_frames, n_icon);

    if(n_steps == 0)
        n_steps = n_icon;
    if(n_steps > CB_MAX_STEPS)
        n_steps = CB_MAX_STEPS;

    for(s = 0; s < n_steps; s++) {
        cb_frame      f;
        unsigned long idx = s;
        unsigned long jif;
        char          tag[600];

        if(seq && s < n_seq)
            idx = rd_u32(seq + s * 4);
        if(idx >= n_icon) {
            fprintf(stderr, "curbake: %s: step %lu references frame %lu of "
                    "%lu, clamped\n", what, s, idx, n_icon);
            idx = n_icon - 1;
        }

        snprintf(tag, sizeof tag, "%s[%lu]", what, s);
        if(cur_decode(icon[idx], icon_len[idx], tag, &f) != 0)
            return -1;

        jif        = (rate && s < n_rate) ? rd_u32(rate + s * 4) : disp_rate;
        f.delay_ms = jiffies_to_ms(jif);

        if(role_push(role, &f) != 0) {
            free(f.px);
            fprintf(stderr, "curbake: out of memory\n");
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */

static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE          *f = fopen(path, "rb");
    long           n;
    unsigned char *b;

    if(!f)
        return NULL;
    if(fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
       fseek(f, 0, SEEK_SET) != 0 || (unsigned long)n > CB_MAX_FILE) {
        fclose(f);
        return NULL;
    }
    b = (unsigned char *)malloc((size_t)n + 1);
    if(!b) {
        fclose(f);
        return NULL;
    }
    if(n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return b;
}

static void str_lower(char *s)
{
    for(; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

static int has_ext(const char *lower_name, const char *ext)
{
    size_t n = strlen(lower_name), e = strlen(ext);

    return n > e && strcmp(lower_name + n - e, ext) == 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char   *in_dir, *out_path;
    DIR          *dir;
    struct dirent *de;
    char        **names = NULL;
    int           n_names = 0, cap_names = 0;
    cb_role       roles[CB_N_ROLES];
    FILE         *out;
    int           i, r, k;
    unsigned long total_frames = 0, pix_base, off;

    if(argc != 3) {
        fprintf(stderr, "usage: %s <input-dir> <output.psc>\n", argv[0]);
        return 2;
    }
    in_dir   = argv[1];
    out_path = argv[2];

    memset(roles, 0, sizeof roles);

    dir = opendir(in_dir);
    if(!dir) {
        fprintf(stderr, "curbake: cannot open directory '%s'\n", in_dir);
        return 1;
    }
    while((de = readdir(dir)) != NULL) {
        char *dup;

        if(de->d_name[0] == '.')
            continue;
        if(n_names == cap_names) {
            int    nc = cap_names ? cap_names * 2 : 32;
            char **nn = (char **)realloc(names, (size_t)nc * sizeof *nn);

            if(!nn) {
                closedir(dir);
                fprintf(stderr, "curbake: out of memory\n");
                return 1;
            }
            names     = nn;
            cap_names = nc;
        }
        dup = (char *)malloc(strlen(de->d_name) + 1);
        if(!dup) {
            closedir(dir);
            fprintf(stderr, "curbake: out of memory\n");
            return 1;
        }
        strcpy(dup, de->d_name);
        names[n_names++] = dup;
    }
    closedir(dir);

    /* readdir order is filesystem-dependent; sorting keeps the bake byte-for-
     * byte reproducible and makes duplicate-match warnings deterministic.
     */
    qsort(names, (size_t)n_names, sizeof *names, cmp_str);

    for(i = 0; i < n_names; i++) {
        char           lower[512];
        char           path[1024];
        int            is_ani, role = -1;
        unsigned char *data;
        size_t         len;

        snprintf(lower, sizeof lower, "%s", names[i]);
        str_lower(lower);

        is_ani = has_ext(lower, ".ani");
        if(!is_ani && !has_ext(lower, ".cur"))
            continue;

        for(k = 0; k < CB_N_MATCH; k++) {
            if(strstr(lower, g_match[k].needle)) {
                role = g_match[k].role;
                break;
            }
        }
        if(role < 0) {
            fprintf(stderr, "curbake: no role for '%s', skipped\n", names[i]);
            continue;
        }
        if(roles[role].n != 0) {
            fprintf(stderr, "curbake: role %s already filled by '%s', "
                    "skipping '%s'\n", g_role_name[role], roles[role].src,
                    names[i]);
            continue;
        }

        snprintf(path, sizeof path, "%s/%s", in_dir, names[i]);
        data = slurp(path, &len);
        if(!data) {
            fprintf(stderr, "curbake: cannot read '%s'\n", path);
            return 1;
        }

        if(is_ani) {
            if(ani_decode(data, len, names[i], &roles[role]) != 0) {
                free(data);
                return 1;
            }
        } else {
            cb_frame f;

            if(cur_decode(data, len, names[i], &f) != 0) {
                free(data);
                return 1;
            }
            if(role_push(&roles[role], &f) != 0) {
                free(data);
                fprintf(stderr, "curbake: out of memory\n");
                return 1;
            }
        }
        snprintf(roles[role].src, sizeof roles[role].src, "%s", names[i]);
        free(data);
    }

    for(r = 0; r < CB_N_ROLES; r++)
        total_frames += (unsigned long)roles[r].n;

    if(total_frames == 0) {
        fprintf(stderr, "curbake: no cursors matched in '%s'\n", in_dir);
        return 1;
    }

    /* ---- write ---------------------------------------------------------- */

    out = fopen(out_path, "wb");
    if(!out) {
        fprintf(stderr, "curbake: cannot write '%s'\n", out_path);
        return 1;
    }

    pix_base = 16UL + CB_N_ROLES * 16UL + total_frames * 16UL;

    /* Header. The magic is emitted as the characters P,S,C,1 so it reads as
     * 0x31435350 when loaded as a little-endian u32.
     */
    put_u32(out, 0x31435350UL);
    put_u32(out, 1UL);
    put_u32(out, (unsigned long)CB_N_ROLES);
    put_u32(out, total_frames);

    off = 0;
    for(r = 0; r < CB_N_ROLES; r++) {
        put_u32(out, (unsigned long)roles[r].n);
        put_u32(out, off);
        put_u32(out, 0UL);
        put_u32(out, 0UL);
        off += (unsigned long)roles[r].n;
    }

    off = pix_base;
    for(r = 0; r < CB_N_ROLES; r++) {
        for(i = 0; i < roles[r].n; i++) {
            const cb_frame *f = &roles[r].f[i];

            put_u16(out, (unsigned)f->w);
            put_u16(out, (unsigned)f->h);
            put_u16(out, (unsigned)(f->hot_x & 0xffff));
            put_u16(out, (unsigned)(f->hot_y & 0xffff));
            put_u32(out, f->delay_ms);
            put_u32(out, off);
            off += (unsigned long)f->w * (unsigned long)f->h * 2UL;
        }
    }

    for(r = 0; r < CB_N_ROLES; r++) {
        for(i = 0; i < roles[r].n; i++) {
            const cb_frame *f = &roles[r].f[i];
            long            n = (long)f->w * (long)f->h, j;

            for(j = 0; j < n; j++)
                put_u16(out, f->px[j]);
        }
    }

    if(ferror(out)) {
        fprintf(stderr, "curbake: write error on '%s'\n", out_path);
        fclose(out);
        return 1;
    }
    fclose(out);

    /* ---- report --------------------------------------------------------- */

    printf("curbake: %s -> %s\n", in_dir, out_path);
    printf("  %lu bytes, %lu frames across %d roles\n",
           off, total_frames, CB_N_ROLES);
    printf("  %-4s %-16s %-7s %-9s %-9s %s\n",
           "id", "role", "frames", "size", "hotspot", "delays(ms)");

    for(r = 0; r < CB_N_ROLES; r++) {
        char size[32] = "-", hot[32] = "-", delays[256] = "-";

        if(roles[r].n > 0) {
            const cb_frame *f0 = &roles[r].f[0];
            size_t          pos = 0;
            int             uniform = 1;

            snprintf(size, sizeof size, "%dx%d", f0->w, f0->h);
            snprintf(hot, sizeof hot, "%d,%d", f0->hot_x, f0->hot_y);

            for(i = 1; i < roles[r].n; i++) {
                if(roles[r].f[i].delay_ms != f0->delay_ms)
                    uniform = 0;
            }
            if(roles[r].n == 1 && f0->delay_ms == 0) {
                snprintf(delays, sizeof delays, "static");
            } else if(uniform) {
                snprintf(delays, sizeof delays, "%lux%d",
                         f0->delay_ms, roles[r].n);
            } else {
                delays[0] = '\0';
                for(i = 0; i < roles[r].n && pos < sizeof delays - 8; i++)
                    pos += (size_t)snprintf(delays + pos, sizeof delays - pos,
                                            "%s%lu", i ? "," : "",
                                            roles[r].f[i].delay_ms);
            }
        }

        printf("  %-4d %-16s %-7d %-9s %-9s %s\n",
               r, g_role_name[r], roles[r].n, size, hot, delays);
    }

    for(r = 0; r < CB_N_ROLES; r++) {
        for(i = 0; i < roles[r].n; i++)
            free(roles[r].f[i].px);
        free(roles[r].f);
    }
    for(i = 0; i < n_names; i++)
        free(names[i]);
    free(names);

    return 0;
}
