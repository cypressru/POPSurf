/* A voice backend that is memory and a counter, so the streaming logic can be
 * tested without a Dreamcast.
 *
 * What ps_adxstream.c actually does is arithmetic on a play cursor: decide
 * which part of a ring is safe to overwrite, and write it before the hardware
 * gets there. That is entirely testable here - the AICA contributes nothing to
 * it but the cursor - and it is the part most likely to be subtly wrong, since
 * the symptom of an off-by-one on real hardware is a periodic click that could
 * equally be the decoder, the pitch or the transfer.
 *
 * The alignment rules are enforced rather than ignored: the real backend
 * refuses a write that is not a multiple of 32 bytes from a 32-byte aligned
 * source, and a test that quietly accepted one would pass while the console
 * played silence.
 */
#include "ps_voice.h"
#include "fakevoice.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Enough for several start/stop cycles in one program: handles are never
 * reused here, so a test that plays three tracks in a row needs three times
 * what one track holds. */
#define FAKE_MAX_SMP 32

static uint8_t *fake_mem[FAKE_MAX_SMP];
static size_t   fake_len[FAKE_MAX_SMP];
static int      fake_used;

int fake_pos;              /* what ps_voice_pos reports */
int fake_keyed;            /* voices started since the last reset */
int fake_killed;
int fake_bad_write;        /* writes the real backend would have refused */
int fake_uploads;

fake_voiceinfo fake_voice[FAKE_MAX_SLOT];

void fake_voice_reset(void)
{
    int i;

    for(i = 0; i < FAKE_MAX_SMP; i++) {
        free(fake_mem[i]);
        fake_mem[i] = NULL;
        fake_len[i] = 0;
    }
    fake_used     = 0;
    fake_pos      = 0;
    fake_keyed    = 0;
    fake_killed   = 0;
    fake_bad_write = 0;
    fake_uploads  = 0;
    memset(fake_voice, 0, sizeof fake_voice);
}

/* Records what a voice was programmed with. Which registers the AICA would
 * have taken is not interesting here; what a caller got wrong - a loop flag,
 * a sample length, a rate - is, and none of it is visible from the ring. */
static void fake_program(int slot, ps_smp s, int fmt, int loop,
                         uint32_t loop_start, uint32_t frames, uint32_t freq,
                         int vol, int pan)
{
    fake_voiceinfo *v;

    if(slot < 0 || slot >= FAKE_MAX_SLOT)
        return;
    v = &fake_voice[slot];
    v->live       = 1;
    v->smp        = s;
    v->fmt        = fmt;
    v->loop       = loop;
    v->loop_start = loop_start;
    v->frames     = frames;
    v->freq       = freq;
    v->vol        = vol;
    v->pan        = pan;
}

const uint8_t *fake_voice_ring(ps_smp s, size_t *len)
{
    if(s == PS_SMP_NONE || (int)s > fake_used)
        return NULL;
    if(len)
        *len = fake_len[s - 1];
    return fake_mem[s - 1];
}

int ps_voice_init(int want)
{
    return want;
}

/* An upload is an allocation whose contents arrive with it, so it lands in the
 * same table and is read back the same way. The real backend pads to a
 * multiple of 32 bytes and bounces an unaligned source, neither of which
 * changes what a later read sees, so neither is modelled. */
ps_smp ps_voice_upload(const void *data, size_t len)
{
    ps_smp s;

    if(!data || !len)
        return PS_SMP_NONE;

    s = ps_voice_alloc(len);
    if(s == PS_SMP_NONE)
        return PS_SMP_NONE;

    memcpy(fake_mem[s - 1], data, len);
    fake_uploads++;
    return s;
}

void ps_voice_shutdown(void)
{
}

ps_smp ps_voice_alloc(size_t len)
{
    if(fake_used >= FAKE_MAX_SMP)
        return PS_SMP_NONE;

    fake_mem[fake_used] = (uint8_t *)calloc(1, len);
    if(!fake_mem[fake_used])
        return PS_SMP_NONE;
    fake_len[fake_used] = len;

    /* Handles start at one; zero is the null handle. */
    return (ps_smp)(++fake_used);
}

int ps_voice_write(ps_smp s, uint32_t off, const void *data, size_t len)
{
    if(s == PS_SMP_NONE || (int)s > fake_used || !data || !len)
        return -1;

    if((off & 31) || (len & 31) || ((uintptr_t)data & 31)) {
        fake_bad_write++;
        return -1;
    }
    if((size_t)off + len > fake_len[s - 1]) {
        fake_bad_write++;
        return -1;
    }

    memcpy(fake_mem[s - 1] + off, data, len);
    return 0;
}

void ps_voice_release(ps_smp s)
{
    if(s == PS_SMP_NONE || (int)s > fake_used)
        return;
    free(fake_mem[s - 1]);
    fake_mem[s - 1] = NULL;
    fake_len[s - 1] = 0;
}

void ps_voice_arm(int slot, ps_smp s, int fmt, int loop, uint32_t loop_start,
                  uint32_t frames, uint32_t freq, int vol, int pan)
{
    fake_program(slot, s, fmt, loop, loop_start, frames, freq, vol, pan);
}

void ps_voice_play(int slot, ps_smp s, int fmt, int loop, uint32_t loop_start,
                   uint32_t frames, uint32_t freq, int vol, int pan)
{
    fake_program(slot, s, fmt, loop, loop_start, frames, freq, vol, pan);
    fake_keyed++;
}

void ps_voice_key(int slot)
{
    (void)slot;
    fake_keyed++;
}

int ps_voice_pos(int slot)
{
    (void)slot;
    return fake_pos;
}

void ps_voice_kill(int slot)
{
    if(slot >= 0 && slot < FAKE_MAX_SLOT)
        fake_voice[slot].live = 0;
    fake_killed++;
}

void ps_voice_stop(int slot)
{
    (void)slot;
}

void ps_voice_set_level(int slot, int vol, int pan)
{
    if(slot < 0 || slot >= FAKE_MAX_SLOT)
        return;
    fake_voice[slot].vol = vol;
    fake_voice[slot].pan = pan;
    fake_voice[slot].levels++;
}
