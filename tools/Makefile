# Host-side asset tools, built with the system compiler.

CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra

BIN     := curbake

# Where the extracted Windows cursor pack lives; override on the command line.
CURSOR_SRC ?= cursors
CURSOR_OUT := ../cd/cursors.psc

all: $(BIN)

curbake: curbake.c
	$(CC) $(CFLAGS) -o $@ $<

# Re-bake the cursor asset: make -C tools cursors CURSOR_SRC=/path/to/pack
cursors: curbake
	./curbake "$(CURSOR_SRC)" "$(CURSOR_OUT)"

clean:
	rm -f $(BIN)

.PHONY: all cursors clean
