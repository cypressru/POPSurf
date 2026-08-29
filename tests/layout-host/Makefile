# Host build of litehtml plus a layout probe. Nothing here is for the
# Dreamcast: it exists so layout questions can be answered in under a second
# with exact numbers, instead of by uploading to hardware and squinting.
#
#   make && ./layouttest cell-height.html
LH   := ../../vendor/litehtml
TC   :=

CXXFLAGS := -std=c++17 -g -O0 -Wall \
            -I$(LH)/include -I$(LH)/include/litehtml -I$(LH)/src \
            -I$(LH)/src/gumbo/include -I$(LH)/src/gumbo/include/gumbo \
            
CFLAGS   := -std=c23 -g -O0 \
            -I$(LH)/src/gumbo/include -I$(LH)/src/gumbo/include/gumbo

SRCS   := $(wildcard $(LH)/src/*.cpp) main.cpp
CSRCS  := $(wildcard $(LH)/src/gumbo/*.c)
OBJS   := $(patsubst %.cpp,obj/%.o,$(notdir $(SRCS)))
COBJS  := $(patsubst %.c,obj/gumbo_%.o,$(notdir $(CSRCS)))

VPATH := $(LH)/src:$(LH)/src/gumbo:.

all: layouttest

obj:
	@mkdir -p obj


obj/%.o: %.cpp | obj
	$(CXX) $(CXXFLAGS) -c $< -o $@

obj/gumbo_%.o: %.c | obj
	$(CC) $(CFLAGS) -c $< -o $@

layouttest: $(OBJS) $(COBJS)
	$(CXX) -o $@ $^

clean:
	-rm -rf obj layouttest

.PHONY: all clean
