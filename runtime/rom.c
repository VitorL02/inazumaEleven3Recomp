/* ROM: SHA-1, cabeçalho, tabela de overlays e descompressão BLZ ("LZ de trás para frente" da Nintendo). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rom.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
int g_rom_skip_sha;

Rom g_romfile;
#define EXPECTED_CODE "BOEJ"

/* ---------- SHA-1 (domínio público, compacto) ---------- */
static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
void sha1_hex(const uint8_t *d, size_t n, char out[41]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha1_block(h, d + i);
    uint8_t tail[128] = {0}; size_t r = n - i;
    memcpy(tail, d + i, r); tail[r] = 0x80;
    size_t tl = r + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha1_block(h, tail); if (tl == 128) sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) sprintf(out + k * 8, "%08x", h[k]);
}

/* ---------- BLZ ---------- */
int blz_decompress(const uint8_t *in, size_t len, uint8_t **out, size_t *outlen) {
    if (len < 8) return -1;
    uint32_t a, inc; memcpy(&a, in + len - 8, 4); memcpy(&inc, in + len - 4, 4);
    if (inc == 0) { *out = malloc(len); memcpy(*out, in, len); *outlen = len; return 0; }
    uint32_t enc = a & 0xFFFFFF, hdr = a >> 24;
    if (enc > len || hdr > enc) return -1;
    size_t dec = len + inc;
    uint8_t *o = malloc(dec);
    memcpy(o, in, len);
    size_t pak = len - hdr, pak_end = len - enc, raw = dec;
    while (pak > pak_end) {
        uint8_t flags = o[--pak];
        for (int bit = 0; bit < 8 && pak > pak_end; bit++, flags <<= 1) {
            if (flags & 0x80) {
                uint32_t v = (uint32_t)o[pak - 1] << 8 | o[pak - 2]; pak -= 2;
                uint32_t n = (v >> 12) + 3, disp = (v & 0xFFF) + 3;
                while (n-- && raw > 0) { raw--; o[raw] = o[raw + disp]; }
            } else {
                o[--raw] = o[--pak];
            }
        }
    }
    *out = o; *outlen = dec;
    return 0;
}

/* ---------- ROM ---------- */
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static char g_rom_path[1024];
int rom_open(const char *path, char *err, size_t errsz) {
    if (g_romfile.data && !strcmp(path, g_rom_path)) return 0; /* já carregada e validada */
    if (g_romfile.data) { munmap(g_romfile.data, g_romfile.size); memset(&g_romfile, 0, sizeof g_romfile); g_rom_path[0] = 0; }
    /* mapeada, não copiada: 512 MB só entram na memória conforme o jogo lê */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, errsz, "não consegui abrir o arquivo"); return -1; }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0x4000) { close(fd); snprintf(err, errsz, "arquivo pequeno demais para ser uma ROM de DS"); return -1; }
    Rom *r = &g_romfile;
    void *m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { snprintf(err, errsz, "não consegui ler o arquivo"); return -1; }
    r->data = m; r->size = st.st_size;
    if (memcmp(r->data + 12, EXPECTED_CODE, 4)) {
        snprintf(err, errsz, "esta ROM não é Inazuma Eleven 3 (código %.4s, esperado %s)", (char *)r->data + 12, EXPECTED_CODE);
        munmap(r->data, r->size); r->data = NULL;
        return -1;
    }
    if (g_rom_skip_sha) memcpy(r->sha1, "a70e9a336f6ea8ff78003938dcd024533f034a68", 41); /* captura da arte: o launcher já conferiu */
    else sha1_hex(r->data, r->size, r->sha1);
    const uint8_t *h = r->data;
    r->arm9_off = u32(h + 0x20); r->arm9_entry = u32(h + 0x24); r->arm9_ram = u32(h + 0x28); r->arm9_size = u32(h + 0x2C);
    r->fat_off = u32(h + 0x48);
    uint32_t ovt = u32(h + 0x50), ovt_size = u32(h + 0x54);
    r->num_overlays = ovt_size / 32;
    if (r->num_overlays > 256) r->num_overlays = 256;
    for (int i = 0; i < r->num_overlays; i++) {
        const uint8_t *e = r->data + ovt + i * 32;
        RomOverlay *o = &r->ov[i];
        o->id = u32(e); o->ram = u32(e + 4); o->size = u32(e + 8); o->bss = u32(e + 12);
        o->sinit_start = u32(e + 16); o->sinit_end = u32(e + 20); o->file_id = u32(e + 24); o->comp = u32(e + 28);
    }
    snprintf(g_rom_path, sizeof g_rom_path, "%s", path);
    return 0;
}

static uint8_t *g_ovimg[256]; static size_t g_ovimg_size[256];
const uint8_t *rom_overlay_image(int id, size_t *size) {
    if (!g_ovimg[id]) {
        Rom *r = &g_romfile; RomOverlay *o = &r->ov[id];
        uint32_t start = u32(r->data + r->fat_off + o->file_id * 8), end = u32(r->data + r->fat_off + o->file_id * 8 + 4);
        const uint8_t *src = r->data + start; size_t len = end - start;
        if (o->comp & 0x01000000) {
            if (blz_decompress(src, len, &g_ovimg[id], &g_ovimg_size[id])) { fprintf(stderr, "overlay %d: BLZ inválido\n", id); exit(1); }
        } else { g_ovimg[id] = malloc(len); memcpy(g_ovimg[id], src, len); g_ovimg_size[id] = len; }
    }
    *size = g_ovimg_size[id];
    return g_ovimg[id];
}
