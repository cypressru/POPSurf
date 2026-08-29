/* CRT-safe palette and dimensions for browser chrome. */
#ifndef PS_THEME_H
#define PS_THEME_H

#include "ps_types.h"

/* --- Palette ------------------------------------------------------------ */

#define PS_C_ACCENT      PS_ARGB(255, 255, 194,  75)
#define PS_C_ACCENT_DIM  PS_ARGB(255, 150, 112,  40)

#define PS_C_PANEL       PS_ARGB(238,  22,  24,  32)
#define PS_C_PANEL_EDGE  PS_ARGB(255,  64,  68,  86)
#define PS_C_SCRIM       PS_ARGB(170,   8,   9,  13)

#define PS_C_TEXT        PS_ARGB(255, 226, 228, 236)
#define PS_C_TEXT_DIM    PS_ARGB(255, 140, 145, 160)
#define PS_C_TEXT_ON_ACC PS_ARGB(255,  20,  18,  10)

/* Outline colour for anything drawn over unknown page content. Paired with a
 * light fill it stays visible on any background, which matters because the
 * page owns most of the screen. */
#define PS_C_OUTLINE     PS_ARGB(255,  10,  10,  14)

/* --- Y2K metal ---------------------------------------------------------- */

/* The keyboard's keys, the toolbar's buttons and every panel behind them are
 * one material, so its colours live here rather than in whichever file first
 * needed them. They were the on-screen keyboard's private constants until the
 * toolbar wanted the same look; two copies of a material is how two things
 * that should match stop matching.
 *
 * Four bands, not a ramp: a dark rim, a bright upper half falling to mid, a
 * darker lower half rising back as a reflected bounce, and a specular line
 * under the top edge. Aqua and the chrome widgets of that era are all built
 * this way, and the hard step at the midpoint is the look rather than an
 * artefact - which is lucky, because a single smooth ramp is exactly what
 * 16bpp cannot do. */

#define PS_C_METAL_RIM   PS_ARGB(255,  18,  20,  28)
#define PS_C_METAL_HI_A  PS_ARGB(255, 246, 248, 252)
#define PS_C_METAL_HI_B  PS_ARGB(255, 176, 184, 200)
#define PS_C_METAL_LO_A  PS_ARGB(255, 118, 126, 144)
#define PS_C_METAL_LO_B  PS_ARGB(255, 186, 194, 210)

/* Selected keys take the accent but keep the same band structure, so the metal
 * reads as one material throughout. */
#define PS_C_METAL_ON_HI_A PS_ARGB(255, 255, 236, 186)
#define PS_C_METAL_ON_HI_B PS_ARGB(255, 255, 198,  84)
#define PS_C_METAL_ON_LO_A PS_ARGB(255, 198, 140,  28)
#define PS_C_METAL_ON_LO_B PS_ARGB(255, 240, 196, 104)

/* Unavailable keeps the shape and drops the shine. A greyed control that still
 * looks pressable is a worse lie than one that plainly cannot be used. */
#define PS_C_METAL_OFF_A PS_ARGB(255,  96, 100, 112)
#define PS_C_METAL_OFF_B PS_ARGB(255,  72,  76,  88)

#define PS_C_METAL_INK     PS_ARGB(255,  18,  20,  28)
#define PS_C_METAL_INK_ON  PS_ARGB(255,  30,  22,   4)
#define PS_C_METAL_INK_OFF PS_ARGB(255, 132, 136, 148)

#define PS_C_SPECULAR    PS_ARGB(210, 255, 255, 255)

/* Panel body behind the metal, and the recessed well a value sits in. */
#define PS_C_PANEL_RIM   PS_ARGB(255,  14,  16,  22)
#define PS_C_PANEL_HI    PS_ARGB(245,  78,  84,  98)
#define PS_C_PANEL_LO    PS_ARGB(245,  34,  38,  48)

#define PS_C_WELL_RIM    PS_ARGB(255,  10,  12,  16)
#define PS_C_WELL_HI     PS_ARGB(255,  20,  23,  30)
#define PS_C_WELL_LO     PS_ARGB(255,  40,  45,  58)

/* --- Form widgets ------------------------------------------------------- */

/* These are drawn *into the page*, not into our chrome, so they read as
 * neutral controls rather than as POPSurf furniture: a page's form should not
 * look like our menu. Still bound by the CRT rules above - the "white" field
 * is 232, not 255, because pure white blooms. */

#define PS_C_FIELD_BG     PS_ARGB(255, 232, 233, 238)
#define PS_C_FIELD_TEXT   PS_ARGB(255,  24,  26,  32)
#define PS_C_FIELD_EDGE   PS_ARGB(255,  88,  92, 106)
#define PS_C_FIELD_SHADOW PS_ARGB(255, 150, 154, 168)

#define PS_C_BTN_FACE     PS_ARGB(255, 198, 201, 212)
#define PS_C_BTN_HI       PS_ARGB(255, 232, 233, 238)
#define PS_C_BTN_LO       PS_ARGB(255, 118, 122, 136)
#define PS_C_BTN_TEXT     PS_ARGB(255,  24,  26,  32)

/* Focus is shown by an accent ring rather than a colour change: page authors
 * pick the field colours, and we cannot recolour a control without sometimes
 * making it unreadable. */
#define PS_C_FOCUS        PS_C_ACCENT

#define PS_CTL_BOX     20   /* checkbox and radio, square */
#define PS_CTL_PAD_X   6
#define PS_CTL_PAD_Y   4
#define PS_CTL_COLS    20   /* default text field width, in characters */
#define PS_CTL_ROWS    2    /* default textarea height, in lines */

/* --- Metrics ------------------------------------------------------------ */

/* Title-safe inset. TVs eat 5-10% of each edge; chrome inside this is
 * guaranteed visible, page content is not required to be. */
#define PS_SAFE_X 32
#define PS_SAFE_Y 24

/* Minimum structural stroke, in pixels. See the interlace note above. */
#define PS_STROKE 2

/* A cursor-on-a-stick is imprecise; rows need to be forgiving. */
#define PS_ROW_H  40

#define PS_PAD    12

/* UI type sizes.
 *
 * These were 16 and 22, sized the way a desktop UI is sized. That is the wrong
 * reference: 480i on a composite signal loses most of a 16px glyph's detail to
 * chroma bleed and interlace, and the reader is on a sofa rather than at a
 * desk. Everything went up a step, and the toolbar - which is the one surface
 * you read at a glance rather than study - takes the larger of the two.
 *
 * The glyph atlas holds PS_CFG_MAX_FONTS sizes at once and does not evict, so
 * the chrome deliberately uses exactly two and leaves the rest for the page. */
#define PS_FONT_UI    20
#define PS_FONT_TITLE 26

/* --- Toolbar ------------------------------------------------------------ */

/* The band the toolbar reserves along the bottom of the screen.
 *
 * Bottom, not top, and that is a rendering decision as much as a taste one:
 * the document is painted from y=0 with its own clip, so a band taken off the
 * bottom leaves every page, frame and hit test addressed exactly as before,
 * while a band taken off the top would put an origin offset through all three.
 * It also matches what the console browsers of the period did.
 *
 * The band runs to the physical bottom of the picture, so no sliver of page
 * shows underneath it, but a TV eats the bottom 5-10% - so the row of controls
 * has to finish on the title-safe line and the remainder below is panel colour
 * bleeding into the overscan.
 *
 * That skirt is not spacing and must not be counted as spacing. The row is
 * centred in the part of the band a viewer can actually see - rule to
 * title-safe line - which is why the margin appears twice here and the skirt
 * appears once. Centring in the whole band instead would push the controls
 * down into the part of the picture the tube throws away. */
#define PS_BAR_ROW_H  44
#define PS_BAR_RULE   PS_STROKE
#define PS_BAR_MARGIN 8
#define PS_BAR_BAND_H (PS_BAR_RULE + PS_BAR_MARGIN + PS_BAR_ROW_H + \
                       PS_BAR_MARGIN + PS_SAFE_Y)

/* --- Screensaver -------------------------------------------------------- */

/* A CRT with a static toolbar burnt into the bottom of it is a permanently
 * damaged television, and this one draws the same amber rule in the same
 * hundred pixels for as long as the browser is open. Thirty seconds is what
 * kos-tool's screensaver uses and there is no reason to differ. */
#define PS_SAVER_IDLE_MS 30000

/* The screen goes black and the mark is cut out of it: the apple is a hole you
 * see the page through, not a shape drawn on top of one.
 *
 * This is the strongest of the three arrangements tried, and by accident. A
 * veil over the whole page keeps every pixel lit and only dims them; a mark
 * painted on black is a bright shape wandering over a dark field. A mask is
 * both at once - ninety-six percent of the tube is off, and the small part
 * that is lit shows different content every second as the window travels. */
#define PS_C_SAVER_MASK PS_ARGB(255, 0, 0, 0)

/* Source bitmap is 32x32 at one bit per pixel; it is drawn at two and a half
 * times that. The scale is a ratio rather than a float because the span edges
 * are computed in integers - col * DRAW / ICON - which makes every span abut
 * its neighbour exactly, with no seam and no overlap at the half pixel. */
#define PS_SAVER_ICON  32
#define PS_SAVER_DRAW  80

#define PS_SAVER_SPEED  2   /* pixels per 60Hz frame, in each axis */

#endif /* PS_THEME_H */
