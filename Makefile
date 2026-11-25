# Build do protótipo. Precisa de extracted/ (tools/extract.py) e da análise (tools/analyze.py).
CC ?= gcc
CFLAGS ?= -O1 -g -w -fno-strict-aliasing -fwrapv
GEN = build/gen
GEN_SRCS = $(wildcard $(GEN)/*.c)
RT_SRCS = runtime/runtime.c runtime/thumb.c runtime/threads.c runtime/video.c runtime/gx.c runtime/snd.c runtime/rom.c runtime/config.c runtime/launcher.c runtime/main.c
OBJS = $(patsubst %.c,build/obj/%.o,$(notdir $(GEN_SRCS))) $(patsubst %.c,build/obj/rt_%.o,$(notdir $(RT_SRCS)))

all: build/ie3

gen:
	python3 -I recomp/armrecomp.py extracted analysis/functions_list.txt $(GEN)

build/obj/%.o: $(GEN)/%.c runtime/runtime.h runtime/cpu.h
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -Iruntime -I$(GEN) -c $< -o $@

build/obj/rt_%.o: runtime/%.c $(wildcard runtime/*.h)
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -Iruntime -I$(GEN) -c $< -o $@

# Com SDL2 instalado, abre janela: make SDL=1
ifeq ($(SDL),1)
CFLAGS += -DHAVE_SDL2 $(shell sdl2-config --cflags)
LDLIBS += $(shell sdl2-config --libs)
endif

LDLIBS += -lm

build/ie3: $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

clean:
	rm -rf build/obj build/ie3
.PHONY: all gen clean
