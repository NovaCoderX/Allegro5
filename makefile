# Builds lib/liballegro5.a: the subset of the Allegro 5 API that Dune
# Dynasty needs, for Amiga 68k, on SDL2 (SDL2-NovaMesa). The modules not
# implemented yet are stubs (see README.md).
#
#   make                         build lib/liballegro5.a
#   make install [PREFIX=dir]    copy the headers and library under PREFIX
#   make clean
#
# Drop ALLEGRO_STUB_TRACE from CPPFLAGS to silence the stub trace.

CC       := m68k-amigaos-gcc
AR       := m68k-amigaos-ar
SDL2_CFLAGS ?= -I/opt/amiga/include/SDL2
CPPFLAGS := -Iinclude $(SDL2_CFLAGS) -DALLEGRO_STUB_TRACE
CFLAGS   := -O2 -fno-optimize-sibling-calls -Wall -fmessage-length=0 -m68060 -noixemul
PREFIX   ?= /opt/amiga

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=obj/%.o)
LIB  := lib/liballegro5.a

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

install: $(LIB)
	mkdir -p $(PREFIX)/include $(PREFIX)/lib
	cp -R include/allegro5 $(PREFIX)/include/
	cp $(LIB) $(PREFIX)/lib/

clean:
	rm -rf obj lib

-include $(OBJS:.o=.d)

.PHONY: all install clean
