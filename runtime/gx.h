#pragma once
#include <stdint.h>
/* Motor 3D do DS por software: recebe os comandos de geometria (GXFIFO 04000400 e as portas
 * 04000440..040005FF), monta os polígonos e, no VBlank seguinte ao SWAP_BUFFERS, rasteriza a cena
 * num quadro de 256x192 que o compositor 2D usa como BG0 do motor A. */
void gx_write32(uint32_t off, uint32_t v);     /* off = endereço - 04000000 */
uint32_t gx_read32(uint32_t off);              /* GXSTAT, RAM_COUNT e os resultados de teste/matriz */
int gx_fifo_irq(void);                       /* condição da IRQ do GXFIFO */
void gx_vblank(const uint8_t *io);             /* faz a troca de buffers pendente */
/* último quadro 3D: cor 15 bits e alfa 0..31 (0 = transparente) */
extern uint16_t g_gx_color[256 * 192];
extern uint8_t g_gx_alpha[256 * 192];
