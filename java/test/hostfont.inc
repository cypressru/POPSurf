/* stb_truetype text backend, shared by the shape demo and the applet runner.
 * The browser's own TTF, so what these render is the real typeface. */
#ifndef HOSTFONT_INC
#define HOSTFONT_INC

/* --- text backend -------------------------------------------------------- */

typedef struct {
    unsigned char  *ttf;
    stbtt_fontinfo  info;
    int             ok;
} host_font;

static int hf_scaled(host_font *f, int px, float *scale)
{
    if(!f->ok)
        return 0;
    *scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    return 1;
}

static int hf_measure(void *user, const char *s, size_t len, int px)
{
    host_font *f = (host_font *)user;
    float      scale;
    int        w = 0;
    size_t     i;

    if(!hf_scaled(f, px, &scale))
        return 0;

    for(i = 0; i < len; i++) {
        int adv, lsb;

        stbtt_GetCodepointHMetrics(&f->info, (unsigned char)s[i], &adv, &lsb);
        w += (int)(adv * scale);
    }
    return w;
}

static int hf_ascent(void *user, int px)
{
    host_font *f = (host_font *)user;
    float      scale;
    int        a, d, l;

    if(!hf_scaled(f, px, &scale))
        return px;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &l);
    return (int)(a * scale);
}

static int hf_descent(void *user, int px)
{
    host_font *f = (host_font *)user;
    float      scale;
    int        a, d, l;

    if(!hf_scaled(f, px, &scale))
        return 0;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &l);
    return (int)(-d * scale);
}

static void hf_draw(void *user, ps_jsurface *s, int x, int baseline,
                    const char *str, size_t len, uint32_t argb, int px,
                    int cx0, int cy0, int cx1, int cy1)
{
    host_font *f = (host_font *)user;
    float      scale;
    size_t     i;
    int        pen = x;

    if(!hf_scaled(f, px, &scale))
        return;

    for(i = 0; i < len; i++) {
        int            c = (unsigned char)str[i];
        int            gw, gh, gx, gy, adv, lsb, ix, iy;
        unsigned char *bmp;

        bmp = stbtt_GetCodepointBitmap(&f->info, scale, scale, c,
                                       &gw, &gh, &gx, &gy);
        stbtt_GetCodepointHMetrics(&f->info, c, &adv, &lsb);

        if(bmp) {
            for(iy = 0; iy < gh; iy++) {
                int ty = baseline + gy + iy;

                if(ty < cy0 || ty >= cy1 || ty < 0 || ty >= s->h)
                    continue;

                for(ix = 0; ix < gw; ix++) {
                    int      tx = pen + gx + ix;
                    unsigned a  = bmp[iy * gw + ix];
                    uint32_t *d;
                    unsigned  ia;

                    if(!a || tx < cx0 || tx >= cx1 || tx < 0 || tx >= s->w)
                        continue;

                    d  = &s->px[(size_t)ty * s->stride + tx];
                    ia = 255u - a;
                    *d = 0xff000000u
                       | (((((argb >> 16) & 0xff) * a + ((*d >> 16) & 0xff) * ia) / 255u) << 16)
                       | (((((argb >>  8) & 0xff) * a + ((*d >>  8) & 0xff) * ia) / 255u) <<  8)
                       |  ((((  argb        & 0xff) * a + (( *d        & 0xff) * ia)) / 255u));
                }
            }
            stbtt_FreeBitmap(bmp, NULL);
        }
        pen += (int)(adv * scale);
    }
}


static int host_font_load(host_font *f, const char *path)
{
    FILE *fp;
    long  sz;

    memset(f, 0, sizeof *f);
    fp = fopen(path, "rb");
    if(!fp)
        return -1;
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    f->ttf = (unsigned char *)malloc((size_t)sz);
    if(f->ttf && fread(f->ttf, 1, (size_t)sz, fp) == (size_t)sz)
        f->ok = stbtt_InitFont(&f->info, f->ttf, 0);
    fclose(fp);
    return f->ok ? 0 : -1;
}

static void host_font_ops(host_font *f, ps_jtext_ops *ops)
{
    ops->user = f;
    ops->measure = hf_measure;
    ops->draw    = hf_draw;
    ops->ascent  = hf_ascent;
    ops->descent = hf_descent;
}

static void host_font_free(host_font *f) { free(f->ttf); f->ttf = NULL; }

#endif /* HOSTFONT_INC */
