/* Just enough SWF writer to make a movie with sound in it.
 *
 * Shared by the host test, which builds fixtures in memory, and by mksndswf,
 * which writes the files that go on the disc for a hardware run. Those two
 * want the same tags for opposite reasons - one to assert against, one to
 * listen to - and a fixture that is not the same shape as the thing shipped
 * proves less than it looks like it does.
 *
 * tests/swf-host/mkswf writes far more than this and is not reused, because it
 * is the picture suite's and is owned elsewhere; nothing here draws anything it
 * knows about. What is here is the sound tags, plus the smallest shape and
 * placement a person can watch to tell whether a click landed on the frame it
 * was supposed to.
 */
#ifndef SWFBUILD_H
#define SWFBUILD_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *b; size_t n, cap; int bit; unsigned acc; } bw;

void bw_init(bw *w, size_t cap);
void bw_free(bw *w);
void bw_bits(bw *w, uint32_t v, int n);
void bw_sbits(bw *w, int32_t v, int n);
void bw_flush(bw *w);
void bw_u8(bw *w, unsigned v);
void bw_u16(bw *w, unsigned v);
void bw_u32(bw *w, uint32_t v);
void bw_bytes(bw *w, const uint8_t *p, size_t n);

/* Bits needed to hold v as a signed field, which is what every geometry field
 * in this format is measured in. Never fewer than two, because the format's own
 * minimum for an edge is two and a zero-width field is unreadable. */
int bw_sbits_needed(int32_t v);

void swf_tagged(bw *out, int code, const bw *body);
void swf_showframe(bw *tags);
void swf_end(bw *tags);
void swf_bgcolor(bw *tags, unsigned rgb);

/* A solid-filled rectangle as one DefineShape, in twips, with the shape's
 * origin at its top left. */
void swf_rect_shape(bw *tags, int id, int32_t w, int32_t h, unsigned rgb);

/* PlaceObject2. `move` places over whatever is at that depth already, which is
 * how a character is animated without being redefined. */
void swf_place(bw *tags, int depth, int id, int32_t tx, int32_t ty, int move);
void swf_remove(bw *tags, int depth);

/* --- sound --------------------------------------------------------------- */

void swf_define_sound(bw *tags, int id, int fmt, int rate_code, int bits16,
                      int stereo, uint32_t nsample, const uint8_t *d,
                      size_t n);

typedef struct {
    int      stop, no_multiple;
    int      has_in, has_out;
    uint32_t in, out;
    int      loops;              /* 0 for absent */
    int      nenv;
    uint32_t pos44[8];
    uint16_t left[8], right[8];
} swf_startinfo;

void swf_start_sound(bw *tags, int id, const swf_startinfo *si);
void swf_stream_head(bw *tags, int fmt, int rate_code, int stereo, int spf);
void swf_stream_block(bw *tags, const uint8_t *d, size_t n);

/* Flash ADPCM, one packet of up to 4096 samples per call, mono.
 *
 * The encoder exists so a test file can carry the codec real content of this
 * era actually uses. It is greedy - the largest magnitude bit that fits, then
 * the next - which is what every encoder of this format did, and it is exact
 * in the sense that matters here: the decoder's predictor is stepped with the
 * same arithmetic while encoding, so what comes back is what was written and
 * not an approximation of it. */
void swf_adpcm_stream(bw *out, const int16_t *src, uint32_t n, int nbits);

uint8_t *swf_finish(const bw *tags, int32_t stage_w, int32_t stage_h, int fps,
                    int nframe, size_t *out_len);

#endif /* SWFBUILD_H */
