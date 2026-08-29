/* Controls for the stub voice backend in fakevoice.c. */
#ifndef FAKEVOICE_H
#define FAKEVOICE_H

#include "ps_voice.h"

#include <stddef.h>
#include <stdint.h>

extern int fake_pos;         /* set this to move the simulated play cursor */
extern int fake_keyed;
extern int fake_killed;
extern int fake_bad_write;   /* non-zero means a write the AICA would drop */
extern int fake_uploads;

/* Voices are addressed by slot, and slot numbers come from the top of the
 * pool, so this has to be wide enough for the largest PS_CFG_AUDIO_VOICES any
 * profile declares rather than for the caller under test. */
#define FAKE_MAX_SLOT 64

/* What a slot was last programmed with. `live` clears on kill, which is how a
 * test tells "the voice was released" from "the voice was reprogrammed". */
typedef struct {
    int      live;
    ps_smp   smp;
    int      fmt;
    int      loop;
    uint32_t loop_start;
    uint32_t frames;
    uint32_t freq;
    int      vol, pan;
    int      levels;         /* ps_voice_set_level calls since the reset */
} fake_voiceinfo;

extern fake_voiceinfo fake_voice[FAKE_MAX_SLOT];

void           fake_voice_reset(void);
const uint8_t *fake_voice_ring(ps_smp s, size_t *len);

#endif
