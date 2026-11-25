#pragma once
#include <stdint.h>
/* VRAM (bancos A..I), paleta, OAM e os dois motores 2D do DS. */
extern uint8_t g_pal[2 << 10];
extern uint8_t g_oam[2 << 10];
uint8_t *video_vram_ptr(uint32_t addr);   /* NULL se não mapeado */
void video_remap(const uint8_t *io);      /* chamar depois de escrever VRAMCNT */
void video_render(const uint8_t *io, uint32_t *top, uint32_t *bottom); /* 256x192 RGBA cada */
