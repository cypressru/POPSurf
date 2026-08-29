/* Platform-specific memory and capacity limits. */
#ifndef PS_CONFIG_H
#define PS_CONFIG_H

/* ------------------------------------------------------------------ target */

#if !defined(PS_PLATFORM_DC) && !defined(PS_PLATFORM_XBOX) && \
    !defined(PS_PLATFORM_GC) && !defined(PS_PLATFORM_PS2) && \
    !defined(PS_PLATFORM_HOST)
#  if defined(_arch_dreamcast)
#    define PS_PLATFORM_DC 1
#  else
#    define PS_PLATFORM_HOST 1
#  endif
#endif

/* ------------------------------------------------------------- Dreamcast */

#if defined(PS_PLATFORM_DC)

#define PS_PROFILE_NAME     "Dreamcast"

/* Sequencer events held for one tune, at 8 bytes each. */
#define PS_CFG_MIDI_EVENTS  32768

/* Leave AICA channels available for the shell and streaming audio. */
#define PS_CFG_AUDIO_VOICES 40

/* 16 MB total, about 12 MB available after KOS and the executable. */
#define PS_MEM_BUDGET_KB      12288
#define PS_MEM_DOCUMENT_KB     5632   /* DOM, CSS, boxes */
#define PS_MEM_IMAGES_KB       2048
#define PS_MEM_NET_KB          1024

#define PS_CFG_VRAM_KB         8192

/* One live tab. Others freeze to url/title/scroll/form state and refetch on
 * switch, because there is no disk cache to restore them from. */
#define PS_CFG_LIVE_TABS          1
#define PS_CFG_DISK_CACHE         0
#define PS_CFG_PERSISTENT_COOKIES 0

#define PS_CFG_MAX_CONNECTIONS    2
#define PS_CFG_MAX_PAGE_BYTES     (2 * 1024 * 1024)

/* Separate asset limit accommodates the General MIDI bank. */
#define PS_CFG_MAX_ASSET_BYTES    (4 * 1024 * 1024)

/* Compressed ADX is retained in RAM while it streams. */
#define PS_CFG_MAX_ADX_BYTES      (1024 * 1024)
#define PS_CFG_MAX_BODY_BYTES     4096
#define PS_CFG_LOADER_QUEUE       64

#define PS_CFG_MAX_TEXTURES       512
#define PS_CFG_IMAGE_CACHE        64
#define PS_CFG_IMAGE_MAX_DIM      1024
#define PS_CFG_IMAGE_MAX_PIXELS   (1024 * 1024)
#define PS_CFG_IMAGE_MAX_FRAMES   64
#define PS_CFG_IMAGE_TOTAL_PIXELS (2 * 1024 * 1024)

#define PS_CFG_GLYPH_ATLAS        512
#define PS_CFG_MAX_FONTS          4

#define PS_CFG_HISTORY            16

/* ------------------------------------------------------------------ Xbox */

#elif defined(PS_PLATFORM_XBOX)

/* 64MB and a hard disk, so the comfort tier opens right up. Rendering stays
 * identical; only capacity changes. */
#define PS_PROFILE_NAME     "Xbox"

/* Sequencer events held for one tune, at 8 bytes each. */
#define PS_CFG_MIDI_EVENTS  262144

/* DirectSound voice budget. */
#define PS_CFG_AUDIO_VOICES 64

#define PS_MEM_BUDGET_KB      49152
#define PS_MEM_DOCUMENT_KB    16384
#define PS_MEM_IMAGES_KB      12288
#define PS_MEM_NET_KB          4096

#define PS_CFG_VRAM_KB         0      /* UMA */

#define PS_CFG_LIVE_TABS          8
#define PS_CFG_DISK_CACHE         1
#define PS_CFG_PERSISTENT_COOKIES 1

#define PS_CFG_MAX_CONNECTIONS    4
#define PS_CFG_MAX_PAGE_BYTES     (8 * 1024 * 1024)

/* Separate asset limit accommodates the General MIDI bank. */
#define PS_CFG_MAX_ASSET_BYTES    (16 * 1024 * 1024)

/* Compressed ADX is retained in RAM while it streams. */
#define PS_CFG_MAX_ADX_BYTES      (8 * 1024 * 1024)
#define PS_CFG_MAX_BODY_BYTES     65536
#define PS_CFG_LOADER_QUEUE       256

#define PS_CFG_MAX_TEXTURES       2048
#define PS_CFG_IMAGE_CACHE        256
#define PS_CFG_IMAGE_MAX_DIM      2048
#define PS_CFG_IMAGE_MAX_PIXELS   (4 * 1024 * 1024)
#define PS_CFG_IMAGE_MAX_FRAMES   256
#define PS_CFG_IMAGE_TOTAL_PIXELS (16 * 1024 * 1024)

#define PS_CFG_GLYPH_ATLAS        1024
#define PS_CFG_MAX_FONTS          12

#define PS_CFG_HISTORY            64

/* -------------------------------------------------------------- GameCube */

#elif defined(PS_PLATFORM_GC)

/* 24 MB main memory plus 16 MB ARAM. */
#define PS_PROFILE_NAME     "GameCube"

/* Sequencer events held for one tune, at 8 bytes each. */
#define PS_CFG_MIDI_EVENTS  65536

/* Software mixer voice budget. */
#define PS_CFG_AUDIO_VOICES 48

#define PS_MEM_BUDGET_KB      20480
#define PS_MEM_DOCUMENT_KB     8192
#define PS_MEM_IMAGES_KB       4096
#define PS_MEM_NET_KB          1536

#define PS_CFG_ARAM_SPILL_KB  16384
#define PS_CFG_VRAM_KB         1024   /* texture cache, tighter than DC */

#define PS_CFG_LIVE_TABS          1
#define PS_CFG_DISK_CACHE         0
#define PS_CFG_PERSISTENT_COOKIES 0

#define PS_CFG_MAX_CONNECTIONS    2
#define PS_CFG_MAX_PAGE_BYTES     (4 * 1024 * 1024)

/* Separate asset limit accommodates the General MIDI bank. */
#define PS_CFG_MAX_ASSET_BYTES    (8 * 1024 * 1024)

/* Compressed ADX is retained in RAM while it streams. */
#define PS_CFG_MAX_ADX_BYTES      (2 * 1024 * 1024)
#define PS_CFG_MAX_BODY_BYTES     8192
#define PS_CFG_LOADER_QUEUE       96

#define PS_CFG_MAX_TEXTURES       768
#define PS_CFG_IMAGE_CACHE        96
#define PS_CFG_IMAGE_MAX_DIM      1024
#define PS_CFG_IMAGE_MAX_PIXELS   (1024 * 1024)
#define PS_CFG_IMAGE_MAX_FRAMES   96
#define PS_CFG_IMAGE_TOTAL_PIXELS (4 * 1024 * 1024)

#define PS_CFG_GLYPH_ATLAS        512
#define PS_CFG_MAX_FONTS          6

#define PS_CFG_HISTORY            32

/* ------------------------------------------------------------------- PS2 */

#elif defined(PS_PLATFORM_PS2)

/* 32MB main but only 4MB of GS eDRAM, ~1.1MB of which is the framebuffer.
 * Textures live in main RAM and DMA per frame under an upload budget, so the
 * resident texture count is the tight constraint rather than total memory. */
#define PS_PROFILE_NAME     "PlayStation 2"

/* Sequencer events held for one tune, at 8 bytes each. */
#define PS_CFG_MIDI_EVENTS  131072

/* Simultaneous notes. Peak polyphony in real General MIDI files runs into the
 * mid twenties, and a sequencer forced to steal a voice cuts a sounding note
 * dead - audible as a click at the loudest moment, because dense passages are
 * exactly when stealing happens. The SPU2 has 48 voices. */
#define PS_CFG_AUDIO_VOICES 48

#define PS_MEM_BUDGET_KB      28672
#define PS_MEM_DOCUMENT_KB    10240
#define PS_MEM_IMAGES_KB       6144
#define PS_MEM_NET_KB          2048

#define PS_CFG_VRAM_KB         4096

#define PS_CFG_LIVE_TABS          2
#define PS_CFG_DISK_CACHE         0
#define PS_CFG_PERSISTENT_COOKIES 1   /* 8MB memory card */

#define PS_CFG_MAX_CONNECTIONS    2
#define PS_CFG_MAX_PAGE_BYTES     (4 * 1024 * 1024)

/* Ceiling for a single non-page download. The soundbank is the one asset
 * that dwarfs any document - it is a whole GM instrument set - and it has
 * no business being measured against the page budget. */
#define PS_CFG_MAX_ASSET_BYTES    (8 * 1024 * 1024)

/* Ceiling for one streamed audio track, held compressed in main RAM for the
 * life of the page. Decoded audio is bounded by the ring buffer and does not
 * scale with track length; only this does. */
#define PS_CFG_MAX_ADX_BYTES      (4 * 1024 * 1024)
#define PS_CFG_MAX_BODY_BYTES     16384
#define PS_CFG_LOADER_QUEUE       128

#define PS_CFG_MAX_TEXTURES       512
#define PS_CFG_IMAGE_CACHE        128
#define PS_CFG_IMAGE_MAX_DIM      1024
#define PS_CFG_IMAGE_MAX_PIXELS   (2 * 1024 * 1024)
#define PS_CFG_IMAGE_MAX_FRAMES   128
#define PS_CFG_IMAGE_TOTAL_PIXELS (6 * 1024 * 1024)

#define PS_CFG_GLYPH_ATLAS        512
#define PS_CFG_MAX_FONTS          8

#define PS_CFG_HISTORY            32

/* ------------------------------------------------------------------ host */

#else

/* Reference build and golden-image authority. Deliberately generous: when a
 * corpus page fails here it is a real bug, not a budget. The memory-cap
 * harness re-imposes the Dreamcast budget explicitly rather than
 * relying on these. */
#define PS_PROFILE_NAME     "Host"

/* Sequencer events held for one tune, at 8 bytes each. */
#define PS_CFG_MIDI_EVENTS  1048576

/* Simultaneous notes. Peak polyphony in real General MIDI files runs into the
 * mid twenties, and a sequencer forced to steal a voice cuts a sounding note
 * dead - audible as a click at the loudest moment, because dense passages are
 * exactly when stealing happens. Reference build; not a hardware limit. */
#define PS_CFG_AUDIO_VOICES 64

#define PS_MEM_BUDGET_KB      262144
#define PS_MEM_DOCUMENT_KB     65536
#define PS_MEM_IMAGES_KB       65536
#define PS_MEM_NET_KB          16384

#define PS_CFG_VRAM_KB         0

#define PS_CFG_LIVE_TABS          16
#define PS_CFG_DISK_CACHE         1
#define PS_CFG_PERSISTENT_COOKIES 1

#define PS_CFG_MAX_CONNECTIONS    6
#define PS_CFG_MAX_PAGE_BYTES     (16 * 1024 * 1024)

/* Ceiling for a single non-page download. The soundbank is the one asset
 * that dwarfs any document - it is a whole GM instrument set - and it has
 * no business being measured against the page budget. */
#define PS_CFG_MAX_ASSET_BYTES    (32 * 1024 * 1024)

/* Ceiling for one streamed audio track, held compressed in main RAM for the
 * life of the page. Decoded audio is bounded by the ring buffer and does not
 * scale with track length; only this does. */
#define PS_CFG_MAX_ADX_BYTES      (16 * 1024 * 1024)
#define PS_CFG_MAX_BODY_BYTES     262144
#define PS_CFG_LOADER_QUEUE       512

#define PS_CFG_MAX_TEXTURES       4096
#define PS_CFG_IMAGE_CACHE        512
#define PS_CFG_IMAGE_MAX_DIM      4096
#define PS_CFG_IMAGE_MAX_PIXELS   (16 * 1024 * 1024)
#define PS_CFG_IMAGE_MAX_FRAMES   512
#define PS_CFG_IMAGE_TOTAL_PIXELS (64 * 1024 * 1024)

#define PS_CFG_GLYPH_ATLAS        1024
#define PS_CFG_MAX_FONTS          16

#define PS_CFG_HISTORY            128

#endif

/* --------------------------------------------------------------- derived */

/* Encoded audio one Flash movie may retain.
 *
 * The same ceiling a streamed track gets, and for the same reason: it is held
 * in main RAM for as long as the page is open, and decoded audio never exists
 * as a unit on either path. It is derived rather than stated per profile
 * because the two are the same question - how much compressed music may one
 * page hold - and two numbers that must agree are one number too many. */
#ifndef PS_CFG_MAX_SWF_AUDIO_BYTES
#define PS_CFG_MAX_SWF_AUDIO_BYTES PS_CFG_MAX_ADX_BYTES
#endif

/* --------------------------------------------------------------- invariant */

/* The carved regions must fit the budget. Catching this at compile time is
 * the point of having one file: a console profile that over-commits should
 * never reach a device with no memory protection. */
typedef char ps_config_budget_fits[
    (PS_MEM_DOCUMENT_KB + PS_MEM_IMAGES_KB + PS_MEM_NET_KB <= PS_MEM_BUDGET_KB)
        ? 1 : -1];

/* A track the streamer would accept but the transport would refuse is a
 * download that always fails at the very end, which is the most expensive way
 * to say no. */
typedef char ps_config_adx_fits[
    (PS_CFG_MAX_ADX_BYTES <= PS_CFG_MAX_ASSET_BYTES) ? 1 : -1];

#endif /* PS_CONFIG_H */
