/* Renderizador 2D dos motores A e B do DS, por linha.
 * Cobre: mapeamento dos bancos de VRAM, BGs de texto, afins e estendidos (bitmap 8/16 bits),
 * sprites (normais e afins, 4/8 bits, 1D/2D, paletas estendidas), prioridades,
 * modo de exibição VRAM, troca de telas e brilho mestre. BG0 3D vem do gx.c. Há blending simples (sem janelas). */
#include <string.h>
#include <stdlib.h>
#include "video.h"
#include "gx.h"

uint8_t g_pal[2 << 10];
uint8_t g_oam[2 << 10];

static const uint32_t bank_size[9] = {128 << 10, 128 << 10, 128 << 10, 128 << 10, 64 << 10, 16 << 10, 16 << 10, 32 << 10, 16 << 10};
static uint8_t *g_bank[9];
/* tabela de páginas de 16 KB para 06000000-06FFFFFF; cada entrada aponta para um banco */
static uint8_t *g_page[1024];
/* paletas estendidas: [motor][0 = BG (4 slots de 8 KB), 1 = OBJ (8 KB)] */
static uint8_t *g_bgext[2][4], *g_objext[2];
/* slots de textura (4 x 128 KB) e de paleta de textura (8 x 16 KB) do motor 3D */
static uint8_t *g_tex[4], *g_texpal[8];

static void map_range(uint32_t base, uint32_t region_size, uint32_t off, int bank) {
    /* mapeia o banco em base+off e espelha dentro da região */
    uint32_t sz = bank_size[bank];
    for (uint32_t mirror = 0; mirror < 0x200000; mirror += region_size) {
        if (region_size == 0) break;
        for (uint32_t p = 0; p < sz; p += 0x4000) {
            uint32_t a = base + mirror + ((off + p) % region_size);
            if (a < 0x06000000 || a >= 0x07000000) continue;
            g_page[(a - 0x06000000) >> 14] = g_bank[bank] + p;
        }
        if (base >= 0x06800000) break; /* LCDC não espelha */
    }
}

void video_remap(const uint8_t *io) {
    for (int i = 0; i < 9; i++) if (!g_bank[i]) g_bank[i] = calloc(1, bank_size[i]);
    memset(g_page, 0, sizeof g_page);
    memset(g_bgext, 0, sizeof g_bgext); memset(g_objext, 0, sizeof g_objext);
    memset(g_tex, 0, sizeof g_tex); memset(g_texpal, 0, sizeof g_texpal);
    static const uint32_t lcdc[9] = {0x6800000, 0x6820000, 0x6840000, 0x6860000, 0x6880000, 0x6890000, 0x6894000, 0x6898000, 0x68A0000};
    for (int b = 0; b < 9; b++) {
        uint8_t cnt = io[0x240 + (b < 7 ? b : b + 1)];
        if (!(cnt & 0x80)) continue;
        int mst = cnt & 7, ofs = (cnt >> 3) & 3;
        if (b <= 3) mst &= 3 | (b >= 2 ? 4 : 0);
        switch (b) {
        case 0: case 1: case 2: case 3:
            if (mst == 0) map_range(lcdc[b], 0x100000, 0, b);
            else if (mst == 1) map_range(0x6000000, 0x80000, 0x20000 * ofs, b);
            else if (mst == 2 && b <= 1) map_range(0x6400000, 0x40000, 0x20000 * (ofs & 1), b);
            else if (mst == 4 && b == 2) map_range(0x6200000, 0x20000, 0, b);
            else if (mst == 4 && b == 3) map_range(0x6600000, 0x20000, 0, b);
            else if (mst == 3) g_tex[ofs] = g_bank[b];
            break;
        case 4:
            if (mst == 0) map_range(lcdc[b], 0x100000, 0, b);
            else if (mst == 1) map_range(0x6000000, 0x80000, 0, b);
            else if (mst == 2) map_range(0x6400000, 0x40000, 0, b);
            else if (mst == 4) for (int s = 0; s < 4; s++) g_bgext[0][s] = g_bank[b] + s * 0x2000;
            else if (mst == 3) for (int s = 0; s < 4; s++) g_texpal[s] = g_bank[b] + s * 0x4000;
            break;
        case 5: case 6: {
            uint32_t o = 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
            if (mst == 0) map_range(lcdc[b], 0x100000, 0, b);
            else if (mst == 1) map_range(0x6000000, 0x80000, o, b);
            else if (mst == 2) map_range(0x6400000, 0x40000, o, b);
            else if (mst == 4) { g_bgext[0][(ofs & 1) * 2] = g_bank[b]; g_bgext[0][(ofs & 1) * 2 + 1] = g_bank[b] + 0x2000; }
            else if (mst == 5) g_objext[0] = g_bank[b];
            else if (mst == 3) g_texpal[(ofs & 1) + (ofs >> 1) * 4] = g_bank[b];
            break; }
        case 7:
            if (mst == 0) map_range(lcdc[b], 0x100000, 0, b);
            else if (mst == 1) map_range(0x6200000, 0x20000, 0, b);
            else if (mst == 2) for (int s = 0; s < 4; s++) g_bgext[1][s] = g_bank[b] + s * 0x2000;
            break;
        case 8:
            if (mst == 0) map_range(lcdc[b], 0x100000, 0, b);
            else if (mst == 1) map_range(0x6200000, 0x20000, 0x8000, b);
            else if (mst == 2) map_range(0x6600000, 0x20000, 0, b);
            else if (mst == 3) g_objext[1] = g_bank[b];
            break;
        }
    }
}

uint8_t *video_tex_ptr(uint32_t off) {
    uint8_t *b = g_tex[(off >> 17) & 3];
    return b ? b + (off & 0x1FFFF) : NULL;
}
uint8_t *video_texpal_ptr(uint32_t off) {
    if (off >= 0x20000) return NULL;
    uint8_t *b = g_texpal[off >> 14];
    return b ? b + (off & 0x3FFF) : NULL;
}
uint8_t *video_vram_ptr(uint32_t a) {
    if (a < 0x06000000 || a >= 0x07000000) return NULL;
    if (a >= 0x06800000 && a >= 0x068A4000) return NULL;
    uint8_t *p = g_page[(a - 0x06000000) >> 14];
    return p ? p + (a & 0x3FFF) : NULL;
}
static uint16_t vram16(uint32_t a) { uint8_t *p = video_vram_ptr(a); if (!p) return 0; uint16_t v; memcpy(&v, p, 2); return v; }
static uint8_t vram8(uint32_t a) { uint8_t *p = video_vram_ptr(a); return p ? *p : 0; }

static uint16_t io16(const uint8_t *io, uint32_t off) { uint16_t v; memcpy(&v, io + off, 2); return v; }
static uint32_t io32(const uint8_t *io, uint32_t off) { uint32_t v; memcpy(&v, io + off, 4); return v; }
static uint16_t pal16(uint32_t off) { uint16_t v; memcpy(&v, g_pal + (off & 0x7FF), 2); return v; }

/* um pixel de camada: cor 15 bits + bit 15 = opaco */
#define OPAQUE 0x8000

typedef struct { uint16_t color[256]; uint8_t prio[256]; } Line;

static void render_text_bg(const uint8_t *io, int eng, int n, int y, Line *out) {
    uint32_t base = eng ? 0x04001000 : 0x04000000, ioff = eng ? 0x1000 : 0;
    uint32_t dispcnt = io32(io, ioff);
    uint16_t cnt = io16(io, ioff + 0x08 + n * 2);
    uint32_t hofs = io16(io, ioff + 0x10 + n * 4) & 0x1FF, vofs = io16(io, ioff + 0x12 + n * 4) & 0x1FF;
    uint32_t vbase = eng ? 0x06200000 : 0x06000000;
    uint32_t charb = vbase + ((cnt >> 2) & 15) * 0x4000 + (eng ? 0 : ((dispcnt >> 24) & 7) * 0x10000);
    uint32_t scrb = vbase + ((cnt >> 8) & 31) * 0x800 + (eng ? 0 : ((dispcnt >> 27) & 7) * 0x10000);
    int c256 = (cnt >> 7) & 1, size = cnt >> 14;
    int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    int extpal = c256 && (dispcnt >> 30) & 1;
    int slot = n; if (n < 2 && (cnt >> 13) & 1) slot = n + 2;
    uint32_t py = (y + vofs) & (h - 1);
    (void)base;
    for (int x = 0; x < 256; x++) {
        uint32_t px = (x + hofs) & (w - 1);
        uint32_t sb = scrb + ((py >> 8) * ((w >> 8)) + (px >> 8)) * 0x800;
        if (w == 512 && h == 512) sb = scrb + (((py >> 8) * 2) + (px >> 8)) * 0x800;
        uint16_t e = vram16(sb + (((py & 255) >> 3) * 32 + ((px & 255) >> 3)) * 2);
        uint32_t tx = px & 7, ty = py & 7;
        if (e & 0x400) tx = 7 - tx;
        if (e & 0x800) ty = 7 - ty;
        uint32_t tile = e & 0x3FF, ci;
        uint16_t col;
        if (c256) {
            ci = vram8(charb + tile * 64 + ty * 8 + tx);
            if (!ci) continue;
            if (extpal) { uint8_t *p = g_bgext[eng][slot]; col = p ? (p[(e >> 12) * 512 + ci * 2] | p[(e >> 12) * 512 + ci * 2 + 1] << 8) : 0; }
            else col = pal16(eng * 0x400 + ci * 2);
        } else {
            uint8_t b = vram8(charb + tile * 32 + ty * 4 + tx / 2);
            ci = (tx & 1) ? b >> 4 : b & 15;
            if (!ci) continue;
            col = pal16(eng * 0x400 + ((e >> 12) * 16 + ci) * 2);
        }
        out->color[x] = col | OPAQUE;
    }
}

static void render_affine_bg(const uint8_t *io, int eng, int n, int y, Line *out, int mode) {
    /* mode: 1 = afim normal (tiles 8 bits, mapa de 8 bits); 2 = estendido */
    uint32_t ioff = eng ? 0x1000 : 0;
    uint32_t dispcnt = io32(io, ioff);
    uint16_t cnt = io16(io, ioff + 0x08 + n * 2);
    uint32_t vbase = eng ? 0x06200000 : 0x06000000;
    int16_t pa = io16(io, ioff + 0x20 + (n - 2) * 0x10), pc = io16(io, ioff + 0x24 + (n - 2) * 0x10);
    int16_t pb = io16(io, ioff + 0x22 + (n - 2) * 0x10), pd = io16(io, ioff + 0x26 + (n - 2) * 0x10);
    int32_t rx = (int32_t)(io32(io, ioff + 0x28 + (n - 2) * 0x10) << 4) >> 4;
    int32_t ry = (int32_t)(io32(io, ioff + 0x2C + (n - 2) * 0x10) << 4) >> 4;
    rx += pb * y; ry += pd * y;
    int wrap = (cnt >> 13) & 1, size = cnt >> 14;
    uint32_t charb = vbase + ((cnt >> 2) & 15) * 0x4000 + (eng ? 0 : ((dispcnt >> 24) & 7) * 0x10000);
    uint32_t scrb_t = vbase + ((cnt >> 8) & 31) * 0x800 + (eng ? 0 : ((dispcnt >> 27) & 7) * 0x10000);
    if (mode == 2 && (cnt & 0x80)) {
        /* bitmap: 256 cores (bit 2 = 0) ou cor direta (bit 2 = 1) */
        static const int bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        int W = bw[size], H = bh[size];
        uint32_t base = vbase + ((cnt >> 8) & 31) * 0x4000;
        int direct = (cnt >> 2) & 1;
        for (int x = 0; x < 256; x++, rx += pa, ry += pc) {
            int px = rx >> 8, py = ry >> 8;
            if (wrap) { px &= W - 1; py &= H - 1; } else if (px < 0 || py < 0 || px >= W || py >= H) continue;
            if (direct) { uint16_t v = vram16(base + (py * W + px) * 2); if (v & 0x8000) out->color[x] = (v & 0x7FFF) | OPAQUE; }
            else { uint8_t ci = vram8(base + py * W + px); if (ci) out->color[x] = pal16(eng * 0x400 + ci * 2) | OPAQUE; }
        }
        return;
    }
    int S = 128 << size;
    int extpal = (dispcnt >> 30) & 1;
    for (int x = 0; x < 256; x++, rx += pa, ry += pc) {
        int px = rx >> 8, py = ry >> 8;
        if (wrap) { px &= S - 1; py &= S - 1; } else if (px < 0 || py < 0 || px >= S || py >= S) continue;
        uint32_t tx = px & 7, ty = py & 7;
        if (mode == 1) {
            uint8_t tile = vram8(scrb_t + (py >> 3) * (S >> 3) + (px >> 3));
            uint8_t ci = vram8(charb + tile * 64 + ty * 8 + tx);
            if (ci) out->color[x] = pal16(eng * 0x400 + ci * 2) | OPAQUE;
        } else {
            uint16_t e = vram16(scrb_t + ((py >> 3) * (S >> 3) + (px >> 3)) * 2);
            if (e & 0x400) tx = 7 - tx;
            if (e & 0x800) ty = 7 - ty;
            uint8_t ci = vram8(charb + (e & 0x3FF) * 64 + ty * 8 + tx);
            if (!ci) continue;
            uint16_t col;
            if (extpal) { uint8_t *p = g_bgext[eng][n]; col = p ? (p[(e >> 12) * 512 + ci * 2] | p[(e >> 12) * 512 + ci * 2 + 1] << 8) : 0; }
            else col = pal16(eng * 0x400 + ci * 2);
            out->color[x] = col | OPAQUE;
        }
    }
}

static void render_objs(const uint8_t *io, int eng, int y, Line *out) {
    uint32_t dispcnt = io32(io, eng ? 0x1000 : 0);
    uint8_t *oam = g_oam + eng * 0x400;
    uint32_t vbase = eng ? 0x06600000 : 0x06400000;
    static const int sw[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const int sh[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    for (int i = 127; i >= 0; i--) { /* do fim para o começo: o menor índice fica por cima */
        uint16_t a0, a1, a2;
        memcpy(&a0, oam + i * 8, 2); memcpy(&a1, oam + i * 8 + 2, 2); memcpy(&a2, oam + i * 8 + 4, 2);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 >> 9) & 1) continue; /* desativado */
        int shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3) continue;
        int w = sw[shape][sz], h = sh[shape][sz];
        int bw = w, bh = h;
        if (affine && (a0 >> 9) & 1) { bw *= 2; bh *= 2; }
        int oy = a0 & 0xFF; if (oy >= 192) oy -= 256;
        int ox = a1 & 0x1FF; if (ox >= 256) ox -= 512;
        int ly = y - oy;
        if (ly < 0 || ly >= bh) continue;
        int mode = (a0 >> 10) & 3;
        if (mode == 2) continue; /* janela de OBJ: ainda não */
        int c256 = (a0 >> 13) & 1;
        uint32_t tile = a2 & 0x3FF, prio = (a2 >> 10) & 3, palno = a2 >> 12;
        int16_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            int g = (a1 >> 9) & 31;
            memcpy(&pa, oam + g * 32 + 6, 2); memcpy(&pb, oam + g * 32 + 14, 2);
            memcpy(&pc, oam + g * 32 + 22, 2); memcpy(&pd, oam + g * 32 + 30, 2);
        }
        for (int lx = 0; lx < bw; lx++) {
            int sx = ox + lx;
            if (sx < 0 || sx >= 256) continue;
            int tx, ty;
            if (affine) {
                int cx = lx - bw / 2, cy = ly - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 >> 12) & 1 ? w - 1 - lx : lx;
                ty = (a1 >> 13) & 1 ? h - 1 - ly : ly;
            }
            uint16_t col; int opaque = 0;
            if (mode == 3) { /* OBJ bitmap 16 bits */
                uint32_t addr;
                if ((dispcnt >> 6) & 1) addr = vbase + tile * (128 << ((dispcnt >> 22) & 1)) + (ty * w + tx) * 2;
                else { int mask = (dispcnt >> 5) & 1 ? 0x1F : 0x0F; int bwid = (dispcnt >> 5) & 1 ? 256 : 128;
                       addr = vbase + ((tile & mask) * 0x10 + (tile & ~mask) * 0x80) + ty * bwid * 2 + tx * 2; }
                uint16_t v = vram16(addr);
                if (!(v & 0x8000)) continue;
                col = v & 0x7FFF; opaque = 1;
            } else {
                uint32_t t;
                if ((dispcnt >> 4) & 1) { /* 1D */
                    uint32_t bound = 32 << ((dispcnt >> 20) & 3);
                    t = vbase + tile * bound + ((ty >> 3) * (w >> 3) + (tx >> 3)) * (c256 ? 64 : 32);
                } else { /* 2D: 32 tiles por linha */
                    t = vbase + (tile + (ty >> 3) * 32 + (tx >> 3) * (c256 ? 2 : 1)) * 32;
                }
                uint32_t ci;
                if (c256) {
                    ci = vram8(t + (ty & 7) * 8 + (tx & 7));
                    if (!ci) continue;
                    if ((dispcnt >> 31) & 1) { uint8_t *p = g_objext[eng]; col = p ? (p[palno * 512 + ci * 2] | p[palno * 512 + ci * 2 + 1] << 8) : 0; }
                    else col = pal16(eng * 0x400 + 0x200 + ci * 2);
                } else {
                    uint8_t b = vram8(t + (ty & 7) * 4 + (tx & 7) / 2);
                    ci = (tx & 1) ? b >> 4 : b & 15;
                    if (!ci) continue;
                    col = pal16(eng * 0x400 + 0x200 + (palno * 16 + ci) * 2);
                }
                opaque = 1;
            }
            if (opaque && prio <= out->prio[sx]) { out->color[sx] = col | OPAQUE; out->prio[sx] = prio; }
        }
    }
}

static uint32_t rgb(uint16_t c, uint16_t bright) {
    int r = (c & 31), g = (c >> 5) & 31, b = (c >> 10) & 31;
    int mode = bright >> 14, f = bright & 31; if (f > 16) f = 16;
    if (mode == 1) { r += (31 - r) * f / 16; g += (31 - g) * f / 16; b += (31 - b) * f / 16; }
    else if (mode == 2) { r -= r * f / 16; g -= g * f / 16; b -= b * f / 16; }
    return 0xFF000000u | ((uint32_t)(r * 255 / 31) << 16) | ((uint32_t)(g * 255 / 31) << 8) | (uint32_t)(b * 255 / 31);
}

static void render_engine(const uint8_t *io, int eng, uint32_t *fb) {
    uint32_t ioff = eng ? 0x1000 : 0;
    uint32_t dispcnt = io32(io, ioff);
    uint16_t bright = io16(io, ioff + 0x6C);
    int dmode = (dispcnt >> 16) & 3;
    if (dmode == 0) { for (int i = 0; i < 256 * 192; i++) fb[i] = 0xFFFFFFFF; return; }
    if (dmode == 2 && !eng) { /* exibição direta de um banco LCDC */
        uint32_t base = 0x06800000 + ((dispcnt >> 18) & 3) * 0x20000;
        for (int i = 0; i < 256 * 192; i++) fb[i] = rgb(vram16(base + i * 2), bright);
        return;
    }
    if (dispcnt & 0x80) { for (int i = 0; i < 256 * 192; i++) fb[i] = 0xFFFFFFFF; return; }
    int bgmode = dispcnt & 7;
    for (int y = 0; y < 192; y++) {
        Line obj; memset(&obj, 0, sizeof obj); memset(obj.prio, 4, sizeof obj.prio);
        Line bg[4]; memset(bg, 0, sizeof bg);
        for (int n = 0; n < 4; n++) {
            if (!((dispcnt >> (8 + n)) & 1)) continue;
            if (n == 0 && !eng && (dispcnt & 8)) { /* BG0 = 3D */
                int hofs = io16(io, 0x10) & 0x1FF; if (hofs & 0x100) hofs -= 0x200;
                for (int x = 0; x < 256; x++) {
                    int sx = x + hofs; if (sx < 0 || sx > 255) continue;
                    int i = y * 256 + sx;
                    if (g_gx_alpha[i]) bg[0].color[x] = g_gx_color[i] | OPAQUE;
                }
                continue;
            }
            int kind = 0; /* 0 texto, 1 afim, 2 estendido */
            if (n == 2) kind = (bgmode == 2 || bgmode == 4) ? 1 : bgmode == 5 ? 2 : bgmode == 6 ? 2 : 0;
            if (n == 3) kind = (bgmode == 1 || bgmode == 2) ? 1 : (bgmode >= 3 && bgmode <= 5) ? 2 : 0;
            if (bgmode == 6 && n != 2) continue;
            if (kind == 0) render_text_bg(io, eng, n, y, &bg[n]);
            else render_affine_bg(io, eng, n, y, &bg[n], kind);
        }
        if ((dispcnt >> 12) & 1) render_objs(io, eng, y, &obj);
        uint16_t backdrop = pal16(eng * 0x400);
        uint16_t bldcnt = io16(io, ioff + 0x50), bldalpha = io16(io, ioff + 0x52), bldy = io16(io, ioff + 0x54);
        int bmode = (bldcnt >> 6) & 3;
        int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = bldy & 31;
        if (eva > 16) eva = 16; if (evb > 16) evb = 16; if (evy > 16) evy = 16;
        for (int x = 0; x < 256; x++) {
            /* as duas camadas visíveis de cima: camada 0-3 = BG, 4 = OBJ, 5 = fundo */
            uint16_t c1 = backdrop, c2 = backdrop; int l1 = 5, l2 = 5;
            for (int p = 0; p <= 3 && l2 == 5; p++) {
                if ((obj.color[x] & OPAQUE) && obj.prio[x] == p) {
                    if (l1 == 5) { c1 = obj.color[x]; l1 = 4; } else { c2 = obj.color[x]; l2 = 4; break; }
                }
                for (int n = 0; n < 4; n++) {
                    if (!((dispcnt >> (8 + n)) & 1)) continue;
                    if ((io16(io, ioff + 0x08 + n * 2) & 3) != p) continue;
                    if (!(bg[n].color[x] & OPAQUE)) continue;
                    if (l1 == 5) { c1 = bg[n].color[x]; l1 = n; } else { c2 = bg[n].color[x]; l2 = n; break; }
                }
            }
            uint16_t out = c1 & 0x7FFF;
            int is3d = l1 == 0 && !eng && (dispcnt & 8);
            if (is3d && g_gx_alpha[y * 256 + x] < 31 && ((bldcnt >> (8 + l2)) & 1)) {
                int a = g_gx_alpha[y * 256 + x] + 1;
                out = (uint16_t)((((c1 & 31) * a + (c2 & 31) * (32 - a)) >> 5) | (((((c1 >> 5) & 31) * a + ((c2 >> 5) & 31) * (32 - a)) >> 5) << 5) |
                                 (((((c1 >> 10) & 31) * a + ((c2 >> 10) & 31) * (32 - a)) >> 5) << 10));
            } else if (bmode && ((bldcnt >> l1) & 1)) {
                int r = c1 & 31, g = (c1 >> 5) & 31, b = (c1 >> 10) & 31;
                if (bmode == 1 && ((bldcnt >> (8 + l2)) & 1)) {
                    r = (r * eva + (c2 & 31) * evb) >> 4; g = (g * eva + ((c2 >> 5) & 31) * evb) >> 4; b = (b * eva + ((c2 >> 10) & 31) * evb) >> 4;
                } else if (bmode == 2) { r += (31 - r) * evy >> 4; g += (31 - g) * evy >> 4; b += (31 - b) * evy >> 4; }
                else if (bmode == 3) { r -= r * evy >> 4; g -= g * evy >> 4; b -= b * evy >> 4; }
                if (r > 31) r = 31; if (g > 31) g = 31; if (b > 31) b = 31;
                out = (uint16_t)(r | g << 5 | b << 10);
            }
            fb[y * 256 + x] = rgb(out, bright);
        }
    }
}

void video_render(const uint8_t *io, uint32_t *top, uint32_t *bottom) {
    uint16_t powcnt = io16(io, 0x304);
    int a_on_top = (powcnt >> 15) & 1;
    render_engine(io, 0, a_on_top ? top : bottom);
    render_engine(io, 1, a_on_top ? bottom : top);
}
