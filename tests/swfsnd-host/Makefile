# Host build of the SWF soundtrack player, against the stub voice backend.
#
#   make check
#
# This is deliberately not part of tests/swf-host, which owns the decoder and
# the renderer and links nothing that talks to a voice. What is here needs
# core/ - the voice interface, the config profile and the slot reservations -
# and swf-host's whole point is that it needs none of that. Keeping the two
# apart is the same argument ps_swf_sound.h makes for walking the tag stream
# twice: a build that draws a picture should not have to link a sound backend.
#
# fakevoice.c is tests/adx-host's and is used unmodified. It was written for
# the ADX streamer's ring arithmetic and models exactly what this needs -
# memory, a cursor, and the 32-byte alignment rule the real backend enforces -
# so a second copy would only be a second thing to keep in step.
CORE   := ../../core
SWF    := ../../swf
FAKE   := ../adx-host

CFLAGS := -std=c23 -g -O1 -Wall -Wextra -I$(CORE) -I$(SWF) -I$(FAKE)

SRCS := main.c swfbuild.c $(FAKE)/fakevoice.c $(SWF)/ps_swf_track.c \
        $(SWF)/ps_swf_sound.c $(SWF)/ps_swf_mem.c

DEPS := swfbuild.h $(SWF)/ps_swf_track.h $(SWF)/ps_swf_sound.h $(CORE)/ps_voice.h \
        $(CORE)/ps_audio.h $(CORE)/ps_config.h $(FAKE)/fakevoice.h

all: swfsndtest swfsndcap mksndswf

# Writes the movies that go on the disc, and their pages. Not part of `check`:
# it produces files for a person to listen to, and a build that rewrote tracked
# content on every run would make every commit look like it changed the disc.
#
#   make discfiles
mksndswf: mksndswf.c swfbuild.c swfbuild.h $(SWF)/ps_swf_sound.h
	$(CC) $(CFLAGS) -o $@ mksndswf.c swfbuild.c -lm

discfiles: mksndswf
	./mksndswf ../../cd/swf

swfsndtest: $(SRCS) $(DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

# The same source with the retention cap turned down to 64KB, so the one
# comparison that guards main memory against a page's choice of movie can be
# tested without building a movie the size of the ceiling.
swfsndcap: $(SRCS) $(DEPS)
	$(CC) $(CFLAGS) -DPS_CFG_MAX_SWF_AUDIO_BYTES=65536 \
	      -DSWFSND_CAP_ONLY=1 -o $@ $(SRCS) -lm

check: swfsndtest swfsndcap
	./swfsndtest
	./swfsndcap
	@echo "--- what the shipped files cost"
	@./swfsndtest ../../cd/swf/t_snd.swf ../../cd/swf/t_snd_a.swf \
	              ../../cd/swf/t_snd_ev.swf ../../cd/swf/t_snd_big.swf \
	    | grep -v '^popsurf'

clean:
	-rm -f swfsndtest swfsndcap mksndswf

.PHONY: all check clean discfiles
