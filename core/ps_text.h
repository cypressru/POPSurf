/* Text rendering and glyph atlases. Shaping is limited to printable Latin. */
#ifndef PS_TEXT_H
#define PS_TEXT_H

#include "ps_types.h"
#include "ps_config.h"
#include "ps_paint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Glyphs baked per font: printable ASCII. */
#define PS_GLYPH_FIRST 32
#define PS_GLYPH_COUNT 96

#define PS_ATLAS_W PS_CFG_GLYPH_ATLAS
#define PS_ATLAS_H PS_CFG_GLYPH_ATLAS

/* Distinct sizes/styles resident at once. Each costs one VRAM atlas. */
#define PS_MAX_FONTS PS_CFG_MAX_FONTS

typedef struct {
    int ascent;      /* px above baseline, positive */
    int descent;     /* px below baseline, positive */
    int height;      /* line height */
    int x_height;
} ps_font_metrics;

typedef struct ps_font ps_font;

/* Owns the TTF bytes and every baked atlas. */
typedef struct ps_text_cache ps_text_cache;

ps_text_cache *ps_text_create(const ps_gfx_backend *gfx,
                              const void *ttf, size_t ttf_len);
void           ps_text_destroy(ps_text_cache *tc);

/* Returns NULL when the font table is full or the atlas will not bake. A NULL
 * font is survivable: callers fall back to a font they already have. */
ps_font *ps_text_font(ps_text_cache *tc, int px_size);

void ps_font_get_metrics(const ps_font *f, ps_font_metrics *out);

/* Advance width of a UTF-8 run, in whole pixels. */
int ps_font_measure(const ps_font *f, const char *utf8, size_t len);

/* Draws with (x, y_baseline) as the origin, like CSS. */
void ps_font_draw(ps_paint *p, const ps_font *f, int x, int y_baseline,
                  const char *utf8, size_t len, ps_color color);

/* CPU text rendering for Java applet framebuffers. */
void ps_text_blit(ps_text_cache *tc, uint32_t *dst, int dst_w, int dst_h,
                  int stride, int x, int baseline, const char *utf8,
                  size_t len, uint32_t argb, int px_size,
                  int cx0, int cy0, int cx1, int cy1);

int ps_text_measure_px(ps_text_cache *tc, const char *utf8, size_t len,
                       int px_size);
int ps_text_ascent_px(ps_text_cache *tc, int px_size);
int ps_text_descent_px(ps_text_cache *tc, int px_size);

#ifdef __cplusplus
}
#endif

#endif /* PS_TEXT_H */
