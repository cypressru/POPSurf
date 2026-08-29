/* Minimal AICA sound test - does a bare KOS program make noise on this
 * Dreamcast?
 *
 * KosBrowser plays MIDI correctly under flycast and is completely silent on
 * real hardware. Everything above the hardware checks out: the sequencer
 * reports thousands of notes played, sample data reads back from sound RAM
 * byte-for-byte, and every channel register offset matches KOS's own
 * arm/aica.c. Two independent paths to the chip - our SH-4 register writes and
 * KOS's snd_sfx, which posts commands to the ARM7 - are equally silent.
 *
 * That leaves three possibilities, and the browser cannot distinguish them
 * because it contains too much. So this contains almost nothing:
 *
 *   - No PVR. No network stack. No threads beyond what KOS starts itself.
 *   - INIT_DEFAULT rather than INIT_DEFAULT | INIT_NET, deliberately. With
 *     INIT_NET, KOS moves dcload's console onto its own network stack and the
 *     BBA shares the G2 bus with the AICA - and g2_lock suspends BBA DMA with
 *     interrupts off for every register write. Dropping it removes that
 *     contention entirely, which is the difference this test exists to probe.
 *
 * If this makes noise, the fault is something the browser does - most likely
 * G2 contention - and the next step is to bisect from here towards it.
 *
 * If this is silent, the browser is irrelevant to the bug. Then the register
 * dump below says whether the writes are even landing, and tone B says
 * whether the ARM7 driver came up: KOS's snd_sfx is fire-and-forget, so if
 * the firmware upload was corrupted, every call succeeds and returns a valid
 * handle while nothing reaches the hardware.
 *
 * Build: KOS_BASE set, then make. Run: dc-tool-ip -t <ip> -l -x aicatest.elf
 * The -l is mandatory on this KOS checkout or fs_dclsocket hangs at boot.
 */
#include <kos.h>
#include <dc/g2bus.h>
#include <dc/spu.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <stdlib.h>
#include <malloc.h>

/* Build with -DWITH_NET to bring KOS's network stack up before touching the
 * AICA. That is the single difference between this test and the browser, and
 * with the plain build audible it is the thing left to prove: with INIT_NET,
 * KOS claims the BBA, which shares the G2 bus with the AICA, and g2_lock
 * suspends BBA DMA with interrupts off around every register write. */
#ifdef WITH_NET
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);
#else
KOS_INIT_FLAGS(INIT_DEFAULT);
#endif

#define AICA_REGS      0xa0700000
#define AICA_MASTERVOL (AICA_REGS + 0x2800)
#define CHNREG(c, r)   (AICA_REGS + 0x80 * (c) + (r))

#define CR_PLAY     0x00
#define CR_SA_LOW   0x04
#define CR_LOOPST   0x08
#define CR_LOOPEND  0x0c
#define CR_AEG      0x10
#define CR_AEG2     0x14
#define CR_FNS_OCT  0x18
#define CR_PAN      0x24
#define CR_DISDL    0x25
#define CR_LPF      0x28
#define CR_VOL      0x29

#define CR_KEYONEX  0x8000
#define CR_KEYONB   0x4000
#define CR_LOOPFLAG 0x0200

#define RATE   22050
#define HZ     440
#define PERIOD (RATE / HZ)
#define FRAMES (PERIOD * 20)      /* whole periods, so the loop joins cleanly */

/* Keys one channel by hand, the same sequence the browser uses. */
static void play_direct(int ch, uint32_t addr)
{
    g2_lock_scoped();
    g2_fifo_wait();

    g2_write_32(CHNREG(ch, CR_PLAY),
                (g2_read_32(CHNREG(ch, CR_PLAY)) & ~CR_KEYONB) | CR_KEYONEX);
    g2_fifo_wait();

    g2_write_32(CHNREG(ch, CR_LOOPST),  0);
    g2_write_32(CHNREG(ch, CR_LOOPEND), FRAMES & 0xffff);
    /* 22050Hz is exactly 44100 * 2^-1, so octave -1 with a zero mantissa. */
    g2_write_32(CHNREG(ch, CR_FNS_OCT), ((-1) << 11) | 0);
    g2_fifo_wait();

    /* Paired into 32-bit accesses rather than four byte writes.
     *
     * KOS's aica.c writes these as bytes, but it runs on the ARM7, which is
     * wired straight to the AICA. From the SH-4 the same stores cross the G2
     * bus, and byte writes do not survive the trip: the register dump showed
     * DISDL reading back 0x00 after a g2_write_8 of 0x0f, while every 32-bit
     * write landed intact. DISDL is the direct send level, so a dropped write
     * there means the channel is configured perfectly and routed nowhere -
     * silence with nothing visibly wrong.
     *
     * SH-4 is little-endian, so the low byte of each 32-bit store lands at the
     * lower address: DIPAN at 0x24 with DISDL at 0x25, LPF Q at 0x28 with TL
     * at 0x29. */
    g2_write_32(CHNREG(ch, CR_PAN),  (0x0f << 8) | 0);     /* DISDL | DIPAN */
    g2_write_32(CHNREG(ch, CR_LPF),  (0    << 8) | 0x24);  /* TL | filter off */
    g2_fifo_wait();

    g2_write_32(CHNREG(ch, CR_AEG),  0x1f);   /* no envelope */
    g2_write_32(CHNREG(ch, CR_AEG2), 0x1f);
    g2_write_32(CHNREG(ch, CR_SA_LOW), addr & 0xffff);
    g2_fifo_wait();

    /* PCMS 01 = 8-bit, plus the high address bits, loop, key on. */
    g2_write_32(CHNREG(ch, CR_PLAY),
                CR_KEYONEX | CR_KEYONB | CR_LOOPFLAG |
                (1u << 7) | (addr >> 16));
    g2_fifo_wait();
}

static void dump_regs(int ch)
{
    uint32_t mvol, play, fns;
    uint8_t  vol, disdl, pan;

    g2_lock_scoped();
    g2_fifo_wait();
    mvol = g2_read_32(AICA_MASTERVOL);
    play = g2_read_32(CHNREG(ch, CR_PLAY));
    fns  = g2_read_32(CHNREG(ch, CR_FNS_OCT));
    g2_fifo_wait();
    vol   = g2_read_8(CHNREG(ch, CR_VOL));
    disdl = g2_read_8(CHNREG(ch, CR_DISDL));
    pan   = g2_read_8(CHNREG(ch, CR_PAN));
    g2_fifo_wait();

    printf("aicatest: ch%d play=%04x fns=%04x vol=%02x disdl=%02x pan=%02x "
           "mvol=%04x\n",
           ch, (unsigned)play, (unsigned)fns, vol, disdl, pan, (unsigned)mvol);
}

int main(int argc, char **argv)
{
    int8_t  *pcm;
    int8_t   back[32];
    uint32_t addr;
    sfxhnd_t h;
    int      i, ch, bad = 0, rc;

    (void)argc;
    (void)argv;

    /* dc-load's console is not a tty, so newlib would buffer 4KB and nothing
     * would arrive until it filled - on a hang, the last line printed is then
     * not the last line that ran. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("aicatest: start\n");

    rc = snd_init();
    printf("aicatest: snd_init=%d, spu free %u bytes\n",
           rc, (unsigned)snd_mem_available());
    if(rc < 0)
        return 1;

    pcm = memalign(32, FRAMES);
    if(!pcm)
        return 1;
    for(i = 0; i < FRAMES; i++)
        pcm[i] = ((i % PERIOD) < PERIOD / 2) ? 100 : -100;

    addr = snd_mem_malloc(FRAMES);
    printf("aicatest: sample at spu 0x%08x\n", (unsigned)addr);
    if(!addr)
        return 1;
    spu_memload_sq(addr, pcm, FRAMES);

    /* Proves the transfer landed before blaming anything downstream. */
    spu_memread(back, addr, sizeof back);
    for(i = 0; i < (int)sizeof back; i++)
        if(back[i] != pcm[i])
            bad++;
    printf("aicatest: readback %s (%d/%d differ)\n",
           bad ? "MISMATCH" : "ok", bad, (int)sizeof back);

    /* Master volume written whole, not read-modify-write: a bad read would
     * fold garbage into bits we are not trying to change. */
    {
        g2_lock_scoped();
        g2_fifo_wait();
        g2_write_32(AICA_MASTERVOL, 0x0f);
    }

    ch = snd_sfx_chn_alloc();
    printf("aicatest: channel %d\n", ch);
    if(ch < 0)
        return 1;

    printf("aicatest: TONE A (direct registers) 4s...\n");
    play_direct(ch, addr);
    dump_regs(ch);
    thd_sleep(4000);

    {
        g2_lock_scoped();
        g2_fifo_wait();
        g2_write_32(CHNREG(ch, CR_PLAY),
                    (g2_read_32(CHNREG(ch, CR_PLAY)) & ~CR_KEYONB) | CR_KEYONEX);
    }
    snd_sfx_chn_free(ch);

    /* Deliberately an octave up. Both tones came out of the same buffer, so
     * with the same pitch they are indistinguishable by ear and "I heard one
     * of them" does not say which path worked. Low is ours, high is KOS's. */
    printf("aicatest: TONE B (KOS snd_sfx) 4s - AN OCTAVE HIGHER than A...\n");
    h = snd_sfx_load_raw_buf((char *)pcm, FRAMES, RATE * 2, 8, 1);
    if(h == SFXHND_INVALID) {
        printf("aicatest: TONE B load failed\n");
    }
    else {
        snd_sfx_play(h, 255, 128);
        thd_sleep(4000);
        snd_sfx_unload(h);
    }

    free(pcm);
    printf("aicatest: done - A silent + B silent means the browser is not "
           "involved\n");
    return 0;
}
