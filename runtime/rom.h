#pragma once
#include <stdint.h>
#include <stddef.h>
/* Leitura da ROM do usuário (.nds): cabeçalho, ARM9, tabela de overlays e imagens descomprimidas. */
typedef struct {
    uint32_t id, ram, size, bss, sinit_start, sinit_end, file_id, comp;
} RomOverlay;

typedef struct {
    const uint8_t *data; size_t size;
    char sha1[41];
    uint32_t arm9_off, arm9_entry, arm9_ram, arm9_size;
    uint32_t fat_off;
    int num_overlays;
    RomOverlay ov[256];
} Rom;

extern Rom g_romfile;
extern int g_rom_skip_sha;
/* Carrega e valida. Retorna 0 se ok; senão escreve o motivo em err. */
int rom_open(const char *path, char *err, size_t errsz);
/* Imagem descomprimida do overlay (alocada uma vez e guardada). */
const uint8_t *rom_overlay_image(int id, size_t *size);
void sha1_hex(const uint8_t *d, size_t n, char out[41]);
int blz_decompress(const uint8_t *in, size_t len, uint8_t **out, size_t *outlen);
