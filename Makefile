# POPSurf - Dreamcast build.
#
# LTO is off on purpose: KOS_CFLAGS turns on -flto=auto -ffat-lto-objects,
# which across 59 litehtml TUs makes every build painfully slow for no benefit
# we can currently measure, and it has bitten dcload in other projects here.
# Revisit once footprint is being tuned for real.

TARGET     := popsurf.elf
BUILD      := build
LITEHTML   := vendor/litehtml

PS_INC := -Icore -Igfx -Inet -Ijava -Iswf
LH_INC := -I$(LITEHTML)/include -I$(LITEHTML)/include/litehtml -I$(LITEHTML)/src \
          -I$(LITEHTML)/src/gumbo/include -I$(LITEHTML)/src/gumbo/include/gumbo

# Build options live here and are given to both compilers. They used to be
# appended to PS_CFLAGS alone, while PS_CXXFLAGS was built from scratch - so
# every -D silently missed the one C++ translation unit in the project, and a
# diagnostic added to ps_document.cpp compiled away to nothing while appearing
# to be enabled.
PS_DEFS     :=
PS_CFLAGS   := -fno-lto $(PS_INC)

# Hardware testing over dc-load has no disc in the drive, and dcload's /pc
# fileserver goes away as soon as KOS claims the BBA for its own net stack, so
# the boot assets have nowhere to come from. Point this at a directory URL on
# the development machine and they are fetched over HTTP instead:
#
#   make ASSET_BASE=http://development-host:8080/cd/
#
# Leave it unset for a disc build; the fallback compiles out entirely.
ifdef ASSET_BASE
PS_DEFS += -DPS_ASSET_BASE_URL=\"$(ASSET_BASE)\"
endif

# The fallback soundbank, used when a page does not name one of its own. A page
# supplies the tune; the bank supplies the instruments to render it with, and
# no public host serves our baked .psb format, so during development it comes
# from wherever we happen to be serving it:
#
#   make SOUNDBANK=http://development-host:8099/gmbank.psb
ifdef SOUNDBANK
PS_DEFS += -DPS_SOUNDBANK_URL=\"$(SOUNDBANK)\"
endif

# Keys a fixed tone at startup and verifies the sample survived the trip into
# sound RAM. Diagnostic build only:
#
#   make SELFTEST=1
ifdef SELFTEST
PS_DEFS += -DPS_AUDIO_SELFTEST=1
endif

# Loads Chart.class off the disc, interprets it, and prints a checksum of the
# frame it drew. Everything about the Java subsystem was developed on a host,
# which is a good way to be wrong about a 32-bit SH-4 with a reluctant FPU:
#
#   make JAVA_SELFTEST=1
ifdef JAVA_SELFTEST
PS_DEFS += -DPS_JAVA_SELFTEST=1
endif

# Where the browser lands at boot. Handy for pointing a hardware run straight
# at whatever is being worked on instead of clicking through:
#
#   make HOME=file:///pc/applet.html
# Per-phase timing for the applet path, printed once a second. The four phases
# have very different shapes and only one of them is ever the problem:
#
#   make APPLET_PROFILE=1
# Forces the staging upload path, so the pixel pack and the VRAM transfer can
# be timed apart. They are interleaved in the fast path and indistinguishable
# there.
# Paints applets as geometry instead of pixels. Measured slower as it stands -
# see the note in ps_applet.c - and switchable rather than deleted:
#
#   make APPLET_VECTOR=1
ifdef APPLET_VECTOR
PS_DEFS += -DPS_APPLET_VECTOR=1
endif

ifdef APPLET_STAGED
PS_DEFS += -DPS_APPLET_STAGED=1
endif

ifdef APPLET_PROFILE
PS_DEFS += -DPS_APPLET_PROFILE=1 -DPS_PAINT_PROFILE=1
endif

# The Flash movie's per-frame cost is printed once a second and needs no flag.
# It is the measurement the feature is blocked on, it is one line, and it only
# appears on a page that carries a movie.

ifdef HOME_URL
PS_DEFS += -DPS_HOME_URL=\"$(HOME_URL)\"
endif

# Objects depend on the flags they were built with. Without this, changing a
# -D on the command line silently reuses stale objects, and the build quietly
# disagrees with what you asked for - which is very hard to spot when the
# symptom is behavioural rather than a compile error.
PS_CFLAGS += $(PS_DEFS)

PS_FLAGSTAMP := $(BUILD)/.psflags
$(shell mkdir -p $(BUILD); \
        printf '%s' '$(PS_CFLAGS)' | cmp -s - $(PS_FLAGSTAMP) 2>/dev/null || \
        printf '%s' '$(PS_CFLAGS)' > $(PS_FLAGSTAMP))
PS_CXXFLAGS := -fno-lto -std=c++17 -fno-rtti -fno-exceptions \
               -include vendor/ps_litehtml_compat.h $(PS_INC) $(LH_INC) \
               $(PS_DEFS)

# litehtml, built once into its own archive.
LH_CXXFLAGS := -fno-lto -std=c++17 -fno-rtti -fno-exceptions \
               -include vendor/ps_litehtml_compat.h $(LH_INC)
LH_CFLAGS   := -fno-lto -std=c99 -Wno-unused-variable \
               -Wno-unused-but-set-variable -Wno-char-subscripts \
               -I$(LITEHTML)/src/gumbo/include \
               -I$(LITEHTML)/src/gumbo/include/gumbo

LH_SRCS  := $(wildcard $(LITEHTML)/src/*.cpp)
LH_CSRCS := $(wildcard $(LITEHTML)/src/gumbo/*.c)
LH_OBJS  := $(patsubst $(LITEHTML)/%.cpp,$(BUILD)/lh/%.o,$(LH_SRCS)) \
            $(patsubst $(LITEHTML)/%.c,$(BUILD)/lh/%.o,$(LH_CSRCS))
LH_LIB   := $(BUILD)/liblitehtml.a

# swf/ is the SWF player's core, shared with the host build under
# tests/swf-host. Everything there is portable C; the two files that are not -
# the span renderer and the triangle backend that rasterises against it - stayed
# behind, because they exist to check this code rather than to run on a console.
# What draws on hardware is gfx/pvr/ps_swf_pvr.c.
PS_CSRCS   := $(wildcard core/*.c) $(wildcard net/*.c) $(wildcard gfx/pvr/*.c) \
              $(wildcard audio/aica/*.c) $(wildcard java/*.c) \
              $(wildcard swf/*.c) $(wildcard shell/*.c)
PS_CXXSRCS := $(wildcard core/*.cpp) $(wildcard shell/*.cpp)
PS_OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(PS_CSRCS)) \
              $(patsubst %.cpp,$(BUILD)/%.o,$(PS_CXXSRCS))

LIBS := -lm

# Assets live on the disc, not in a romdisk. A romdisk is decompressed into
# RAM at boot and stays there for the life of the process, so every byte of it
# is a byte the page cannot use - on a 16MB machine a font and a cursor set
# cost most of a megabyte for nothing. Reading them from the disc filesystem
# costs a seek once at startup and nothing thereafter.
CD_STAGE       := $(BUILD)/disc
CD_STAGE_STAMP := $(BUILD)/.disc-ready
RELEASE_FILES  := cd/index.html cd/sites.html cd/font.ttf cd/cursors.psc
SOUNDBANK_FILE := cd/gmbank.psb
RELEASE_LICENSES := LICENSE LICENSES/Noto-Sans-OFL-1.1.txt \
                    LICENSES/Ruffle-MIT.txt \
                    LICENSES/TimGM6mb-GPL-2.0.txt

all: $(TARGET)



# litehtml's encodings.cpp requires C++ exceptions.
$(BUILD)/lh/src/encodings.o: $(LITEHTML)/src/encodings.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(filter-out -fno-exceptions,$(LH_CXXFLAGS)) -fexceptions -c $< -o $@

$(BUILD)/lh/%.o: $(LITEHTML)/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(LH_CXXFLAGS) -c $< -o $@

$(BUILD)/lh/%.o: $(LITEHTML)/%.c
	@mkdir -p $(dir $@)
	kos-cc $(LH_CFLAGS) -c $< -o $@

$(LH_LIB): $(LH_OBJS)
	@mkdir -p $(dir $@)
	$(KOS_AR) rcs $@ $^

$(BUILD)/%.o: %.c $(PS_FLAGSTAMP)
	@mkdir -p $(dir $@)
	kos-cc $(PS_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.cpp $(PS_FLAGSTAMP)
	@mkdir -p $(dir $@)
	kos-c++ $(PS_CXXFLAGS) -MMD -MP -c $< -o $@

# stb_truetype is compiled as a static single-header implementation. Its full
# API is intentionally present even though POPSurf uses only a subset.
$(BUILD)/core/ps_text.o: PS_CFLAGS += -Wno-unused-function

# Header dependencies, generated by the compiler above. Without these a change
# to a header rebuilds nothing, and the tree links stale objects against new
# declarations - which shows up as behaviour that contradicts the source.
-include $(PS_OBJS:.o=.d)

$(TARGET): $(PS_OBJS) $(LH_LIB)
	kos-c++ -fno-lto -o $@ $(PS_OBJS) $(LH_LIB) $(LIBS)

# The development cd/ tree also carries hardware regression pages. Stage only
# the browser assets so test classes, movies, and diagnostic audio cannot leak
# into a release image. MIDI needs the fallback soundbank; fail instead of
# producing a valid-looking but silent disc when it is missing.
$(CD_STAGE_STAMP): $(RELEASE_FILES) $(SOUNDBANK_FILE) $(RELEASE_LICENSES) Makefile
	rm -rf $(CD_STAGE)
	mkdir -p $(CD_STAGE)/LICENSES
	cp $(RELEASE_FILES) $(CD_STAGE)/
	cp $(SOUNDBANK_FILE) $(CD_STAGE)/
	cp LICENSE $(CD_STAGE)/
	cp LICENSES/*.txt $(CD_STAGE)/LICENSES/
	@touch $@

cdi: $(TARGET) $(CD_STAGE_STAMP)
	mkdcdisc -N -e $(TARGET) -D $(CD_STAGE) -o $(BUILD)/popsurf.cdi

clean:
	rm -rf $(BUILD) $(TARGET)

# Rebuilds ps code but keeps the litehtml archive, which is the slow part.
clean-ps:
	rm -rf $(BUILD)/core $(BUILD)/gfx $(BUILD)/shell $(TARGET)

check-host:
	$(MAKE) -C tests/adx-host
	tests/adx-host/adxtest
	$(MAKE) -C tests/file-host check
	$(MAKE) -C tests/swf-host check
	$(MAKE) -C tests/swfsnd-host check

.PHONY: all cdi clean clean-ps check-host
