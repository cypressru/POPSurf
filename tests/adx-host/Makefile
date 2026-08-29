# Host build of the ADX decoder plus a round-trip test. Nothing here is for
# the Dreamcast: the decoder is portable C99, so its correctness can be settled
# in a second on the development machine instead of by listening to a console
# across the room and guessing whether that noise is the format or the mix.
#
#   make && ./adxtest
#
# The streaming layer is built here too, against the stub voice backend in
# fakevoice.c. Its ring arithmetic is where an off-by-one costs a periodic
# click on hardware and nothing anywhere else, so it is worth having somewhere
# that can answer the question in a second.
CFLAGS := -std=c23 -g -O1 -Wall -Wextra -I../../core

SRCS := main.c fakevoice.c ../../core/ps_adx.c ../../core/ps_adxstream.c

all: adxtest

adxtest: $(SRCS) ../../core/ps_adx.h ../../core/ps_adxstream.h fakevoice.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

clean:
	-rm -f adxtest

.PHONY: all clean
