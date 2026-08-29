# Host build of the URL and local-file logic. Nothing here is for the
# Dreamcast: relative resolution across the network/media boundary is pure
# arithmetic on strings, and every case worth arguing about can be settled in
# a second here instead of by burning a disc and reading a serial log.
#
#   make && ./filetest
#
# C23, matching the Dreamcast build - the main Makefile sets no -std, so GCC's
# gnu23 default applies there. Pinning this to something older would let a
# construct pass here and fail on the console.
CFLAGS := -std=gnu23 -g -O1 -Wall -Wextra -I../../core -I../../net

SRCS := main.c ../../net/ps_url.c ../../net/ps_file.c

all: filetest

filetest: $(SRCS) ../../net/ps_url.h ../../net/ps_file.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

check: filetest
	./filetest

clean:
	-rm -rf filetest fixture

.PHONY: all check clean
