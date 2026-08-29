/* Dreamcast text backend for the applet surface.
 *
 * Bridges ps_jgfx's text vtable onto the browser's own font cache, so an
 * applet's drawString uses the same typeface as the page around it - and
 * without loading a second TTF or linking a second copy of stb_truetype.
 */
#ifndef PS_JDC_H
#define PS_JDC_H

#include "ps_jgfx.h"
#include "ps_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fills in ops so it forwards to tc. ops borrows tc and must not outlive it. */
void ps_jdc_text_ops(ps_jtext_ops *ops, ps_text_cache *tc);

/* Loads an applet off the disc, runs it, and reports what it drew. Built only
 * under PS_JAVA_SELFTEST; see ps_jtest.c for why it exists. */
void ps_java_selftest(ps_text_cache *text);

#ifdef __cplusplus
}
#endif

#endif /* PS_JDC_H */
