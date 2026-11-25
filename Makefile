# Build do protótipo. Precisa de extracted/ (tools/extract.py) e da análise (tools/analyze.py).
#   make SDL=1                      Linux/macOS com janela (precisa de libsdl2-dev)
#   make windows SDL_DIR=<pasta>    ie3.exe para Windows com MinGW (x86_64-w64-mingw32-gcc) e o
#                                   SDL2 "devel mingw" (pasta x86_64-w64-mingw32 do pacote do SDL2)
CC ?= gcc
CFLAGS ?= -O1 -g -w -fno-strict-aliasing -fwrapv
BUILD ?= build
EXE ?=
GEN = build/gen
GEN_SRCS = $(wildcard $(GEN)/*.c)
RT_SRCS = runtime/runtime.c runtime/thumb.c runtime/arm_interp.c runtime/threads.c runtime/video.c runtime/gx.c runtime/snd.c runtime/rom.c runtime/platform.c runtime/hostfiber.c runtime/config.c runtime/launcher.c runtime/main.c
OBJS = $(patsubst %.c,$(BUILD)/obj/%.o,$(notdir $(GEN_SRCS))) $(patsubst %.c,$(BUILD)/obj/rt_%.o,$(notdir $(RT_SRCS)))

all: $(BUILD)/ie3$(EXE)

gen:
	python3 -I recomp/armrecomp.py extracted analysis/functions_list.txt $(GEN)

$(BUILD)/obj/%.o: $(GEN)/%.c runtime/runtime.h runtime/cpu.h
	@mkdir -p $(BUILD)/obj
	$(CC) $(CFLAGS) -Iruntime -I$(GEN) -c $< -o $@

$(BUILD)/obj/rt_%.o: runtime/%.c $(wildcard runtime/*.h)
	@mkdir -p $(BUILD)/obj
	$(CC) $(CFLAGS) -Iruntime -I$(GEN) -c $< -o $@

ifeq ($(WIN),1)
# Windows (MinGW): SDL2 do pacote "devel mingw"; -mwindows não abre console (WIN_CONSOLE=1 abre, para ver o log)
override CFLAGS += -DHAVE_SDL2 -I$(SDL_DIR)/include -I$(SDL_DIR)/include/SDL2 -Dmain=SDL_main
LDLIBS += -L$(SDL_DIR)/lib -lmingw32 -lSDL2main -lSDL2 $(if $(WIN_CONSOLE),-mconsole,-mwindows) -static-libgcc -Wl,--stack,67108864
# ícone, manifesto e dados de versão do .exe
WINDRES ?= $(patsubst %gcc,%windres,$(CC))
OBJS += $(BUILD)/obj/ie3_res.o
$(BUILD)/obj/ie3_res.o: win/ie3.rc win/ie3.ico win/ie3.manifest
	@mkdir -p $(BUILD)/obj
	$(WINDRES) -c 65001 -I win -i win/ie3.rc -o $@
else ifeq ($(SDL),1)
# Com SDL2 instalado, abre janela: make SDL=1
CFLAGS += -DHAVE_SDL2 $(shell sdl2-config --cflags)
LDLIBS += $(shell sdl2-config --libs)
endif

LDLIBS += -lm

$(BUILD)/ie3$(EXE): $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS)
	cp recomplauncher.json $(BUILD)/
ifeq ($(WIN),1)
	cp $(SDL_DIR)/bin/SDL2.dll $(BUILD)/
endif

windows:
	$(MAKE) WIN=1 CC=x86_64-w64-mingw32-gcc BUILD=build-win EXE=.exe CFLAGS="-O1 -w -fno-strict-aliasing -fwrapv" SDL_DIR=$(SDL_DIR)

clean:
	rm -rf build/obj build/ie3 build-win
.PHONY: all gen clean windows
