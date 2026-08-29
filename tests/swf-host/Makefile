# Host build. Nothing here is for the Dreamcast: it exists to answer "what
# would a player actually have to implement" before any of it is written, and
# then to prove the parts that are written are right, in under a second,
# without an upload.
#
#   make            build everything
#   make check      bit reader, bitmap decoder, ActionScript and sound tests,
#                   render the generated files, then check the triangle path
#                   against the span path on all of them
#   make fuzz       the interpreter and then whole malformed files through the
#                   whole path, under ASan, UBSan and LeakSanitizer (see below)
#
#   ./swfdump some.swf              what tags are in it
#   ./swfrender some.swf -o out     shapes to out-NN.ppm
#   ./swfrender some.swf -o out -f all   the movie itself, out-fNNN.ppm
#   ./tricmp some.swf               do the two renderers agree
#   ./ppmcmp shot.ppm out-f000.ppm  does the console agree with this renderer
#                                   (../../tools/dccheck.sh brings the shot back)
#   ./sndtest some.swf              run the sound tests, then inventory a file
#   ./swftrace some.swf             run its ActionScript, print what it traced
#   ./corpus.sh                     the same three over real files off the net
#   ./swffuzz out/t_*.swf           malformed files through parse, display
#                                   list, raster and tessellate. `make fuzz`
#                                   is this with the sanitizers on; see
#                                   swffuzz.c for the corpus and fuzz.sh for
#                                   what is still open
#
# The player's core lives in ../../swf and is built for the Dreamcast too. What
# stays here is host-only by nature: the tools, and the two things that exist to
# check the core rather than to run on hardware - the span renderer, which is
# the oracle, and ps_swf_trisoft.c, which rasterises what the tessellator
# emitted so the two can be compared with no tolerance. Neither will ever be
# compiled for the console, so neither moved.
#
# stb_image.h is included by relative path from the file that needs it, so
# there is no -I for the vendor directory here.
SWF    := ../../swf
CFLAGS := -std=c23 -g -O0 -Wall -Wextra -I$(SWF)
LDLIBS := -lm

CORE := $(SWF)/ps_swf_parse.c $(SWF)/ps_swf_xform.c $(SWF)/ps_swf_geom.c \
        ps_swf_raster.c \
        $(SWF)/ps_swf_tess.c ps_swf_trisoft.c $(SWF)/ps_swf_stage.c \
        $(SWF)/ps_swf_clip.c \
        $(SWF)/ps_swf_mem.c $(SWF)/ps_swf_image.c $(SWF)/ps_swf_morph.c \
        $(SWF)/ps_swf_edit.c

# swfcost counts what would reach the tile accelerator and so links neither
# rasteriser - not the span one and not the triangle one that checks it. It does
# link the playhead, because "one frame" is a question about time.
CST := swfcost.c $(SWF)/ps_swf_parse.c $(SWF)/ps_swf_xform.c \
       $(SWF)/ps_swf_geom.c $(SWF)/ps_swf_tess.c $(SWF)/ps_swf_stage.c \
       $(SWF)/ps_swf_play.c \
       $(SWF)/ps_swf_mem.c $(SWF)/ps_swf_image.c $(SWF)/ps_swf_morph.c \
       $(SWF)/ps_swf_edit.c
# The interpreter is not in CORE: swfrender and tricmp draw pictures and have
# no business running a script, and keeping it out means a fault in it cannot
# be mistaken for a rendering fault. The parser comes the other way, because
# the tests assert that DoAction and DoInitAction land on the right timeline,
# and it now needs the transforms that the display list brought with it.
#
# Three decoders follow the parser wherever it goes, which is why they are in
# both lists: the bitmap one, the morph shape one and the edit text one. None
# of that is a script needing pixels or paragraphs - it is that the tag walk
# calls into all three as it passes their tags, so anything linking
# ps_swf_parse.c links them whether or not it ever draws. Check the link and
# not only the compile when this list changes; the compiler is happy either
# way and the linker is where it shows.
ACT  := actiontest.c $(SWF)/ps_swf_action.c $(SWF)/ps_swf_parse.c \
        $(SWF)/ps_swf_xform.c \
        $(SWF)/ps_swf_mem.c $(SWF)/ps_swf_image.c $(SWF)/ps_swf_morph.c \
        $(SWF)/ps_swf_edit.c
HDRS := $(SWF)/ps_swf.h $(SWF)/ps_swf_bits.h $(SWF)/ps_swf_mem.h \
        $(SWF)/ps_swf_geom.h ps_swf_trisoft.h \
        $(SWF)/ps_swf_action.h $(SWF)/ps_swf_clip.h $(SWF)/ps_swf_image.h \
        $(SWF)/ps_swf_read.h \
        $(SWF)/ps_swf_morph.h $(SWF)/ps_swf_edit.h

# Sound is deliberately outside CORE. It walks the tag stream itself rather than
# hanging off the display-list walk, so nothing that renders a picture needs to
# link a decoder - see the header of ps_swf_sound.h for why the two walks are
# kept apart. It does not go through ps_swf_parse.c either, which is why it is
# the one list with no bitmap decoder in it.
SND := $(SWF)/ps_swf_sound.c $(SWF)/ps_swf_mem.c

# The interpreter plus the parser and nothing that draws: swftrace answers
# "what did this file's script say", which is the only question a third party's
# expected output can be compared against. See corpus.sh.
#
# The three decoders are here for the reason ACT gives - the tag walk calls
# into them whether or not anything ever draws - and not because a script needs
# a pixel.
TRC := swftrace.c $(SWF)/ps_swf_action.c $(SWF)/ps_swf_parse.c \
       $(SWF)/ps_swf_xform.c \
       $(SWF)/ps_swf_mem.c $(SWF)/ps_swf_image.c $(SWF)/ps_swf_morph.c \
       $(SWF)/ps_swf_edit.c

all: swfdump swfrender bittest mkswf ppmcheck tricmp imgtest actiontest \
     sndtest swftrace swfcost ramptest ppmcmp swffuzz

swfcost: $(CST) $(HDRS) $(SWF)/ps_swf_play.h
	$(CC) $(CFLAGS) -o $@ $(CST) $(LDLIBS)

# ramptest links the ramp and the geometry that resolves a fill into a paint,
# and neither rasteriser: what it compares is two ways of evaluating the same
# fill at the same point, which needs no pixels drawn anywhere.
RMP := ramptest.c $(SWF)/ps_swf_ramp.c $(SWF)/ps_swf_geom.c \
       $(SWF)/ps_swf_parse.c $(SWF)/ps_swf_xform.c $(SWF)/ps_swf_mem.c \
       $(SWF)/ps_swf_image.c $(SWF)/ps_swf_morph.c $(SWF)/ps_swf_edit.c

ramptest: $(RMP) $(HDRS) $(SWF)/ps_swf_ramp.h
	$(CC) $(CFLAGS) -o $@ $(RMP) $(LDLIBS)

swfdump: swfdump.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

swfrender: swfrender.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ swfrender.c $(CORE) $(LDLIBS)

tricmp: tricmp.c ps_ppmdiff.c $(CORE) $(HDRS) ps_ppmdiff.h
	$(CC) $(CFLAGS) -o $@ tricmp.c ps_ppmdiff.c $(CORE) $(LDLIBS)

# Links no renderer at all: it compares two pictures that already exist, one of
# them made by a machine this toolchain cannot build for. That is the whole
# reason it is a separate program rather than a mode of tricmp - tricmp renders
# both of its sides itself and has nowhere to put a frame that arrived over the
# network.
ppmcmp: ppmcmp.c ps_ppmdiff.c ps_ppmdiff.h
	$(CC) $(CFLAGS) -o $@ ppmcmp.c ps_ppmdiff.c $(LDLIBS)

bittest: bittest.c $(SWF)/ps_swf_bits.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

imgtest: imgtest.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ imgtest.c $(CORE) $(LDLIBS)

sndtest: sndtest.c $(SND) $(SWF)/ps_swf_sound.h $(SWF)/ps_swf_bits.h $(SWF)/ps_swf_mem.h $(SWF)/ps_swf.h
	$(CC) $(CFLAGS) -o $@ sndtest.c $(SND) $(LDLIBS)

mkswf: mkswf.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

ppmcheck: ppmcheck.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

actiontest: $(ACT) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(ACT) $(LDLIBS)

swftrace: $(TRC) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(TRC) $(LDLIBS)

# The interpreter reads bytecode straight off the network, which makes it the
# most attackable surface in the project, so it gets a sanitizer run of its own.
# -fno-sanitize-recover turns the first fault into a non-zero exit instead of a
# line in a log that a green build would hide.
#
# Separate from `check` on purpose: check has to run wherever the repository
# does, and this needs a toolchain with both sanitizers. The seeds are fixed so
# a failure is reproducible from the banner.
# The include path is here as well as in CFLAGS because this target does not
# use CFLAGS, and the parser it links now pulls in the bitmap decoder - so the
# interpreter's sanitizer run covers stb_image too, which is not a bad place
# for it to end up.
FUZZFLAGS := -std=c23 -g -O1 -Wall -Wextra -I$(SWF) \
             -fsanitize=address,undefined -fno-sanitize-recover=all

# swffuzz is the same idea one level out: whole files rather than one script
# block, and the whole path rather than the interpreter. It links CORE - both
# rasterisers, the display list, the clip - because a file that parses can
# still kill the renderer, and the parsing that arrived after the last ad-hoc
# run is mostly of the kind that produces geometry rather than the kind that
# produces a refusal. What it does with all that is argued in swffuzz.c.
FUZ := swffuzz.c $(CORE)

swffuzz: $(FUZ) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(FUZ) $(LDLIBS)

# abort_on_error puts a sanitizer report through SIGABRT, which is the one
# signal swffuzz keeps for itself: its handler prints the case number, so a
# report arrives with the input that produced it and not only a stack. Leaks
# are asked for by name rather than left to the platform's default, and the
# leakcheck below is what proves asking worked.
FUZZENV := ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
           UBSAN_OPTIONS=abort_on_error=1:print_stacktrace=1

# One command, because a suite that needs three will not be run. Everything
# here is deterministic: the seeds are written down, and any case either tool
# prints can be replayed from its number alone.
#
# The sweep itself is in fuzz.sh rather than here, because -fno-sanitize-recover
# means a known defect stops the run at the case that reaches it and the rest of
# the space never runs. That script resumes past the findings it has a written
# argument for and fails on anything else; the register is the interesting part
# of it and is worth reading before trusting a green line from this target.
fuzz: $(ACT) $(FUZ) $(HDRS) mkswf fuzz.sh
	$(CC) $(FUZZFLAGS) -o actionfuzz $(ACT) $(LDLIBS)
	$(CC) $(FUZZFLAGS) -o swffuzz-san $(FUZ) $(LDLIBS)
	@mkdir -p out
	./mkswf out > /dev/null
	@echo "--- is LeakSanitizer actually switched on"
	@$(FUZZENV) ./swffuzz-san leakcheck > out/leakcheck.log 2>&1; \
	 rc=$$?; \
	 if [ $$rc -eq 0 ]; then \
	     echo "  FAIL: a deliberate 64 KiB leak exited 0. LeakSanitizer is" \
	          "not live, so nothing below is a leak check and the runs that" \
	          "follow cannot claim to be one"; exit 1; \
	 elif grep -q 'LeakSanitizer\|detected memory leaks' out/leakcheck.log; \
	 then echo "  a deliberate leak is reported and exits $$rc"; \
	 else echo "  FAIL: died with $$rc but said nothing about a leak:"; \
	      cat out/leakcheck.log; exit 1; fi
	@echo "--- the interpreter, one script block at a time"
	$(FUZZENV) ./actionfuzz
	$(FUZZENV) ./actionfuzz fuzz 400 1
	$(FUZZENV) ./actionfuzz fuzz 400 20260807
	@echo "--- whole files, whole path"
	./fuzz.sh ./swffuzz-san 1 20260807

# Every expected count below is arithmetic, not a recorded output: at scale 2 a
# 100x60 pixel rectangle is 200*120 filled pixels and there is no judgement in
# it. Five are tolerant, and all five are tolerant for the same reason - where
# an antialiased or discretised boundary falls is not derivable on paper. The
# derivation and the size of each tolerance is argued at the file that produces
# it, in mkswf.c, rather than here.
#
# The bitmap block at the end is the same convention and none of it is
# tolerant, which is worth stating because a decoded picture sounds like the
# kind of thing that needs a tolerance. It does not: a texel boundary is not a
# geometric edge, so nothing is antialiased across it, and every fill in those
# files lands on whole output pixels. The one place a decoded value could have
# been approximate - the JPEG - is restricted to flat grey blocks, where the
# transform is exact in both directions. See mkswf.c for each derivation.
#
# The text layout counts carry a region as well as a total, because a total is
# nearly blind to a layout: three glyphs are three glyphs whether they were
# aligned, and two lines are two lines whether the second one wrapped or ran
# off the side. Counting inside a box the wrong answer would miss is the only
# way to assert where the ink went, and the zero counts among them are what
# fails if alignment is parsed and then ignored.
#
# tricmp then asserts something the counts cannot: that tessellating each of
# these shapes and rasterising the triangles reproduces the span renderer's
# output pixel for pixel. That is the check that makes the triangle path -
# which is the one the PVR will use - testable without a PVR.
#
# The real Sega intro is useful for checking interoperability but is not in the
# repository and cannot be - it is somebody else's content. Point swfrender at
# a local copy by hand.
#
# The composite block at the end is the same discipline applied to files that
# combine features rather than isolating them - a masked sprite of text under a
# colour transform, a button inside a sprite, nested playheads, a gradient that
# is stroked and masked and dimmed at once, a morph inside a mask at three
# ratios, a bitmap under a mask and a transform, an edit text inside a masked
# sprite. Every one of those numbers is still an area or a truncation worked
# out on paper, and only the morph's middle ratio is tolerant - by one column,
# derived, because 32768/65535 is not a half. Everywhere else a boundary would
# have been curved, the mask is placed so the curve is out of shot instead.
# mkswf.c argues each in full.
#
# Two of the files assert a distinction rather than a picture. t_maskgap puts
# both readings of "this mask painted nothing" in one file - a clip over a
# character nothing defines, which is us failing and must be dropped, and a
# clip over a character placed off the stage, which is the file saying so and
# must hide its whole range. One character ID apart, 2400 pixels against 0.
# t_label asserts the two fields that change no pixel at all: a FrameLabel
# resolving to the frame it precedes, and a PlaceObject2 Name surviving a tag
# that also carries a clip depth. Nothing about a picture could show either,
# which is how the parser claimed both for so long without reading them.
#
# What none of it covers is a file written by somebody else's tools, which is
# what corpus.sh is for. That needs the network and so cannot be here.
#
# The swffuzz pass in here is the unsanitized one and is deliberately small:
# it catches a crash or a bound that has come adrift in a second, which is what
# `check` can afford, and it is not a substitute for `make fuzz` - a read that
# merely happens not to fault is invisible without ASan, and every finding this
# suite has produced so far was of exactly that kind. It also keeps the fuzzer
# compiling and linking as the core changes underneath it, which is the way a
# tool that is only run before a release rots.
#
# The ActionScript, sound and bitmap tests are the same idea one level up:
# hand-assembled blocks, hand-computed sample values and hand-built compressed
# streams whose results are arithmetic. imgtest also asserts the other half of
# a decoder, which no picture can show - that a malformed stream is refused,
# for the stated reason, and in the cases that matter without having allocated
# anything first. The fuzz pass here is the unsanitized one, which still
# catches a crash or a bound that has come adrift - `make fuzz` is what catches
# the reads that merely happen not to fault.
check: bittest imgtest swfrender mkswf ppmcheck tricmp actiontest sndtest \
       ramptest swffuzz
	./bittest
	./imgtest
	./actiontest
	./actiontest fuzz 100
	./sndtest
	@mkdir -p out
	./mkswf out
	./swffuzz -n 40 -g 400 out/t_*.swf
	@for f in out/t_*.swf; do \
	    echo "--- $$f"; ./swfrender $$f -o out/`basename $$f .swf` -s 2 \
	        | sed -n 's/^  \(.*edges.*\)/    \1/p'; \
	done
	@echo "--- frames"
	@./swfrender out/t_place.swf -o out/t_place -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_text.swf  -o out/t_text  -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_clip.swf  -o out/t_clip  -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_clipn.swf -o out/t_clipn -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_maskgap.swf -o out/t_maskgap -s 1 -f all \
	    | sed -n 's/^  //p'
	@./swfrender out/t_label.swf -o out/t_label -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_morph.swf -o out/t_morph -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_morphfx.swf -o out/t_morphfx -s 1 -f all \
	    | sed -n 's/^  //p'
	@./swfrender out/t_edit.swf  -o out/t_edit  -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_editwrap.swf -o out/t_editwrap -s 1 -f all \
	    | sed -n 's/^  //p'
	@./swfrender out/t_refuse.swf -o out/t_refuse -s 1 -f all \
	    | sed -n 's/^  //p'
	@./swfrender out/t_xtext.swf -o out/t_xtext -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_xbtn.swf  -o out/t_xbtn  -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_xnest.swf -o out/t_xnest -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_xgrad.swf -o out/t_xgrad -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_xmorph.swf -o out/t_xmorph -s 1 -f all \
	    | sed -n 's/^  //p'
	@./swfrender out/t_xbmp.swf  -o out/t_xbmp  -s 1 -f all | sed -n 's/^  //p'
	@./swfrender out/t_xedit.swf -o out/t_xedit -s 1 -f all | sed -n 's/^  //p'
	@echo "--- pixel counts"
	@fail=0; \
	./ppmcheck out/t_rect-00.ppm    e02020 24000       || fail=1; \
	./ppmcheck out/t_hole-00.ppm    2050d0 43200       || fail=1; \
	./ppmcheck out/t_share-00.ppm   e02020 40000       || fail=1; \
	./ppmcheck out/t_share-00.ppm   20c040 40000       || fail=1; \
	./ppmcheck out/t_circle-00.ppm  8030a0 45239  500  || fail=1; \
	./ppmcheck out/t_layers-00.ppm  3040d0 25600       || fail=1; \
	./ppmcheck out/t_layers-00.ppm  d03030 15600       || fail=1; \
	./ppmcheck out/t_layers-00.ppm  30b040 15600       || fail=1; \
	./ppmcheck out/t_alpha-00.ppm   8f0f0f 26000       || fail=1; \
	./ppmcheck out/t_alpha-00.ppm   470787 6400        || fail=1; \
	./ppmcheck out/t_grad_l-00.ppm  008040 40000       || fail=1; \
	./ppmcheck out/t_grad_l-00.ppm  c02080 40000       || fail=1; \
	./ppmcheck out/t_grad_l-00.ppm  ffff00 12000       || fail=1; \
	./ppmcheck out/t_grad_r-00.ppm  ffc000 31416  200  || fail=1; \
	./ppmcheck out/t_grad_r-00.ppm  0f308f 128584 200  || fail=1; \
	./ppmcheck out/t_grad_r-00.ppm  20e0e0 12000       || fail=1; \
	./ppmcheck out/t_stroke-00.ppm  e03030 9257   150  || fail=1; \
	./ppmcheck out/t_stroke-00.ppm  3080e0 31657  150  || fail=1; \
	./ppmcheck out/t_bowtie-00.ppm  40c060 20000  700  || fail=1; \
	./ppmcheck out/t_place-f000.ppm e02020 800        || fail=1; \
	./ppmcheck out/t_place-f001.ppm e02020 3200       || fail=1; \
	./ppmcheck out/t_place-f002.ppm 701010 3200       || fail=1; \
	./ppmcheck out/t_place-f003.ppm e02020 0          || fail=1; \
	./ppmcheck out/t_text-f000.ppm  40c060 800        || fail=1; \
	./ppmcheck out/t_clip-f000.ppm  e02020 2000       || fail=1; \
	./ppmcheck out/t_clip-f001.ppm  e02020 6000       || fail=1; \
	./ppmcheck out/t_clip-f002.ppm  e02020 0          || fail=1; \
	./ppmcheck out/t_clip-f003.ppm  e02020 6000       || fail=1; \
	./ppmcheck out/t_clip-f004.ppm  e02020 2000       || fail=1; \
	./ppmcheck out/t_clip-f004.ppm  20c040 1600       || fail=1; \
	./ppmcheck out/t_clip-f005.ppm  e02020 4400       || fail=1; \
	./ppmcheck out/t_clip-f005.ppm  20c040 1600       || fail=1; \
	./ppmcheck out/t_clip-f000.ppm  00c0ff 0          || fail=1; \
	./ppmcheck out/t_clip-f005.ppm  00c0ff 0          || fail=1; \
	./ppmcheck out/t_clipn-f000.ppm e02020 300        || fail=1; \
	./ppmcheck out/t_clipn-f000.ppm 00c0ff 0          || fail=1; \
	./ppmcheck out/t_clipn-f001.ppm e02020 3200       || fail=1; \
	./ppmcheck out/t_clipn-f001.ppm 00c0ff 0          || fail=1; \
	./ppmcheck out/t_maskgap-f000.ppm e02020 2400     || fail=1; \
	./ppmcheck out/t_maskgap-f000.ppm 20c040 0        || fail=1; \
	./ppmcheck out/t_maskgap-f001.ppm e02020 0        || fail=1; \
	./ppmcheck out/t_maskgap-f001.ppm 20c040 0        || fail=1; \
	./ppmcheck out/t_label-f000.ppm 3040d0 800        || fail=1; \
	./ppmcheck out/t_label-f003.ppm 3040d0 800        || fail=1; \
	\
	./ppmcheck out/t_bmp-00.ppm       e02020 14400 || fail=1; \
	./ppmcheck out/t_bmp-00.ppm       20c040 1600  || fail=1; \
	./ppmcheck out/t_bmp-00.ppm       3040d0 3200  || fail=1; \
	./ppmcheck out/t_bmp-00.ppm       f0e010 6400  || fail=1; \
	./ppmcheck out/t_bmp-01.ppm       ff0000 14400 || fail=1; \
	./ppmcheck out/t_bmp-01.ppm       00ff00 1600  || fail=1; \
	./ppmcheck out/t_bmp-01.ppm       0000ff 3200  || fail=1; \
	./ppmcheck out/t_bmp-01.ppm       ffffff 6400  || fail=1; \
	./ppmcheck out/t_bmp-02.ppm       118844 14400 || fail=1; \
	./ppmcheck out/t_bmp-02.ppm       cc2266 1600  || fail=1; \
	./ppmcheck out/t_bmp-02.ppm       00aaff 3200  || fail=1; \
	./ppmcheck out/t_bmp-02.ppm       ffee11 6400  || fail=1; \
	./ppmcheck out/t_bmp_a-00.ppm     40c060 14400 || fail=1; \
	./ppmcheck out/t_bmp_a-00.ppm     0f0f8f 1600  || fail=1; \
	./ppmcheck out/t_bmp_a-00.ppm     0f0f0f 6400  || fail=1; \
	./ppmcheck out/t_bmp_a-00.ppm     202020 3844  || fail=1; \
	./ppmcheck out/t_bmp_a-01.ppm     e02020 14400 || fail=1; \
	./ppmcheck out/t_bmp_a-01.ppm     8f8f8f 1600  || fail=1; \
	./ppmcheck out/t_bmp_a-01.ppm     000000 6400  || fail=1; \
	./ppmcheck out/t_bmp_a-01.ppm     202020 3844  || fail=1; \
	./ppmcheck out/t_bmp_fill-00.ppm  e02020 6400  || fail=1; \
	./ppmcheck out/t_bmp_fill-00.ppm  20c040 6400  || fail=1; \
	./ppmcheck out/t_bmp_fill-00.ppm  3040d0 6400  || fail=1; \
	./ppmcheck out/t_bmp_fill-00.ppm  f0e010 6400  || fail=1; \
	./ppmcheck out/t_bmp_fill-01.ppm  e02020 1600  || fail=1; \
	./ppmcheck out/t_bmp_fill-01.ppm  20c040 4800  || fail=1; \
	./ppmcheck out/t_bmp_fill-01.ppm  3040d0 4800  || fail=1; \
	./ppmcheck out/t_bmp_fill-01.ppm  f0e010 14400 || fail=1; \
	./ppmcheck out/t_bmp_fill-02.ppm  e02020 3200  || fail=1; \
	./ppmcheck out/t_bmp_fill-02.ppm  20c040 9600  || fail=1; \
	./ppmcheck out/t_bmp_fill-02.ppm  3040d0 3200  || fail=1; \
	./ppmcheck out/t_bmp_fill-02.ppm  f0e010 9600  || fail=1; \
	./ppmcheck out/t_bmp_fill-03.ppm  8030a0 25600 || fail=1; \
	./ppmcheck out/t_jpeg-00.ppm      303030 6400  || fail=1; \
	./ppmcheck out/t_jpeg-00.ppm      606060 6400  || fail=1; \
	./ppmcheck out/t_jpeg-00.ppm      a0a0a0 6400  || fail=1; \
	./ppmcheck out/t_jpeg-00.ppm      e0e0e0 6400  || fail=1; \
	./ppmcheck out/t_jpeg-01.ppm      303030 6400  || fail=1; \
	./ppmcheck out/t_jpeg-01.ppm      606060 6400  || fail=1; \
	./ppmcheck out/t_jpeg-01.ppm      a0a0a0 6400  || fail=1; \
	./ppmcheck out/t_jpeg-01.ppm      e0e0e0 6400  || fail=1; \
	./ppmcheck out/t_jpeg-02.ppm      303030 6400  || fail=1; \
	./ppmcheck out/t_jpeg-02.ppm      a0a0a0 6400  || fail=1; \
	./ppmcheck out/t_jpeg-02.ppm      404040 6400  || fail=1; \
	./ppmcheck out/t_jpeg-02.ppm      808080 6400  || fail=1; \
	./ppmcheck out/t_morph-f000.ppm e02020 800        || fail=1; \
	./ppmcheck out/t_morph-f001.ppm 808020 2400       || fail=1; \
	./ppmcheck out/t_morph-f002.ppm 20e020 4800       || fail=1; \
	./ppmcheck out/t_morph-f003.ppm 3060f0 2400       || fail=1; \
	./ppmcheck out/t_morph-f003.ppm 20e020 0          || fail=1; \
	./ppmcheck out/t_refuse-f000.ppm e02020 800       || fail=1; \
	./ppmcheck out/t_morphfx-f000.ppm 008040 1000     || fail=1; \
	./ppmcheck out/t_morphfx-f000.ppm c02080 3000     || fail=1; \
	./ppmcheck out/t_morphfx-f001.ppm 008040 2000     || fail=1; \
	./ppmcheck out/t_morphfx-f001.ppm c02080 2000     || fail=1; \
	./ppmcheck out/t_morphfx-f001.ppm e03030 1079 60  || fail=1; \
	exit $$fail
	@echo "--- text layout, by region"
	@fail=0; \
	./ppmcheck out/t_edit-f000.ppm 40c060 1200                     || fail=1; \
	./ppmcheck out/t_edit-f000.ppm 40c060 400 0   0   0  50 40     || fail=1; \
	./ppmcheck out/t_edit-f000.ppm 40c060 400 0  85  40 115 80     || fail=1; \
	./ppmcheck out/t_edit-f000.ppm 40c060 400 0 170  80 202 122    || fail=1; \
	./ppmcheck out/t_edit-f000.ppm 40c060 0   0   0  40  50 122    || fail=1; \
	./ppmcheck out/t_edit-f000.ppm 40c060 0   0 115  40 202 80     || fail=1; \
	./ppmcheck out/t_editwrap-f000.ppm 40c060 2400                 || fail=1; \
	./ppmcheck out/t_editwrap-f000.ppm 40c060 1600 0 0  3 122 23   || fail=1; \
	./ppmcheck out/t_editwrap-f000.ppm 40c060  800 0 0 23 122 43   || fail=1; \
	./ppmcheck out/t_editwrap-f000.ppm 40c060  800 0 0 23  60 43   || fail=1; \
	exit $$fail
	@echo "--- refusals"
	@./swfrender out/t_refuse.swf -o out/t_refuse -f 0 \
	    | grep -q 'characters 1  root' \
	    && echo "  malformed morph and edit text refused, the shape behind" \
	            "them kept" \
	    || { echo "  FAIL: a malformed morph or edit text was accepted"; \
	         exit 1; }
	@echo "--- labels and instance names"
	@./swfrender out/t_label.swf -o out/t_label -f 0 \
	    | grep -q 'label "start" -> frame 0' \
	    && ./swfrender out/t_label.swf -o out/t_label -f 0 \
	    | grep -q 'label "middle" -> frame 2' \
	    && ./swfrender out/t_label.swf -o out/t_label -f 0 \
	    | grep -q 'instance "box" at depth 3' \
	    && echo "  both labels resolve to the frame they precede," \
	            "and the name survives a tag that also carries a clip depth" \
	    || { echo "  FAIL: a FrameLabel or a PlaceObject2 Name was dropped"; \
	         exit 1; }
	@echo "--- composites"
	@fail=0; \
	./ppmcheck out/t_xtext-f000.ppm  1f5f2f 600       || fail=1; \
	./ppmcheck out/t_xtext-f000.ppm  40c060 0         || fail=1; \
	./ppmcheck out/t_xtext-f000.ppm  00c0ff 0         || fail=1; \
	./ppmcheck out/t_xbtn-f000.ppm   e02020 1800      || fail=1; \
	./ppmcheck out/t_xbtn-f000.ppm   20c040 0         || fail=1; \
	./ppmcheck out/t_xbtn-f000.ppm   3040d0 0         || fail=1; \
	./ppmcheck out/t_xbtn-f000.ppm   f0e020 0         || fail=1; \
	./ppmcheck out/t_xnest-f000.ppm  e02020 200       || fail=1; \
	./ppmcheck out/t_xnest-f000.ppm  20c040 0         || fail=1; \
	./ppmcheck out/t_xnest-f000.ppm  3040d0 0         || fail=1; \
	./ppmcheck out/t_xnest-f001.ppm  20c040 800       || fail=1; \
	./ppmcheck out/t_xnest-f001.ppm  e02020 0         || fail=1; \
	./ppmcheck out/t_xnest-f001.ppm  3040d0 0         || fail=1; \
	./ppmcheck out/t_xnest-f002.ppm  3040d0 200       || fail=1; \
	./ppmcheck out/t_xnest-f002.ppm  e02020 0         || fail=1; \
	./ppmcheck out/t_xnest-f002.ppm  20c040 0         || fail=1; \
	./ppmcheck out/t_xnest-f003.ppm  e02020 800       || fail=1; \
	./ppmcheck out/t_xnest-f003.ppm  20c040 0         || fail=1; \
	./ppmcheck out/t_xnest-f003.ppm  3040d0 0         || fail=1; \
	./ppmcheck out/t_xgrad-f000.ppm  004020 3600      || fail=1; \
	./ppmcheck out/t_xgrad-f000.ppm  601040 2400      || fail=1; \
	./ppmcheck out/t_xgrad-f000.ppm  701818 0         || fail=1; \
	./ppmcheck out/t_xgrad-f000.ppm  008040 0         || fail=1; \
	./ppmcheck out/t_xgrad-f001.ppm  701818 3200      || fail=1; \
	./ppmcheck out/t_xgrad-f001.ppm  004020 400       || fail=1; \
	./ppmcheck out/t_xgrad-f001.ppm  601040 400       || fail=1; \
	./ppmcheck out/t_xmorph-f000.ppm 301860 450       || fail=1; \
	./ppmcheck out/t_xmorph-f001.ppm 301860 585       || fail=1; \
	./ppmcheck out/t_xmorph-f002.ppm 301860 900       || fail=1; \
	./ppmcheck out/t_xmorph-f000.ppm 6030c0 0         || fail=1; \
	./ppmcheck out/t_xmorph-f002.ppm 00c0ff 0         || fail=1; \
	./ppmcheck out/t_xbmp-f000.ppm   7f3060 1000      || fail=1; \
	./ppmcheck out/t_xbmp-f000.ppm   206040 800       || fail=1; \
	./ppmcheck out/t_xbmp-f000.ppm   ff60c0 0         || fail=1; \
	./ppmcheck out/t_xbmp-f000.ppm   00c0ff 0         || fail=1; \
	./ppmcheck out/t_xedit-f000.ppm  0f5f3f 400       || fail=1; \
	./ppmcheck out/t_xedit-f000.ppm  0f5f3f 200 0  0 0  23 60 || fail=1; \
	./ppmcheck out/t_xedit-f000.ppm  0f5f3f 200 0 23 0 202 60 || fail=1; \
	./ppmcheck out/t_xedit-f000.ppm  0f5f3f 0   0 43 0 202 60 || fail=1; \
	./ppmcheck out/t_xedit-f000.ppm  20c080 0         || fail=1; \
	./ppmcheck out/t_xedit-f000.ppm  00c0ff 0         || fail=1; \
	exit $$fail
	@echo "--- gradients through the hardware's texture path"
	./ramptest out/t_grad_l.swf out/t_grad_r.swf out/t_xgrad.swf
	@echo "--- triangle path against span path"
	@fail=0; for f in out/t_*.swf; do ./tricmp $$f -s 2 || fail=1; done; exit $$fail
	@echo "--- and again with both sides masked"
	@fail=0; for f in out/t_*.swf; do ./tricmp $$f -s 2 -m || fail=1; done; exit $$fail

clean:
	-rm -f swfdump swfrender bittest mkswf ppmcheck tricmp imgtest swfcost \
	       ramptest ppmcmp \
	       actiontest actionfuzz sndtest swftrace swffuzz swffuzz-san
	-rm -rf out

.PHONY: all check clean fuzz
