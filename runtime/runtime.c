/* Runtime do recomp: memória, I/O mínimo, despacho de funções, SWI, CP15 e HLE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "thumb.h"
#include "overlays.h"
#include "video.h"

uint8_t g_main[4 << 20];
uint8_t g_itcm[32 << 10];
uint8_t g_dtcm[16 << 10];
static uint8_t g_wram[32 << 10];
static uint8_t g_io[0x2000];
const uint8_t *rt_io(void) { return g_io; }
static uint32_t g_vcount;
uint64_t g_frame;
int g_trace;

extern const FuncEntry g_funcs[];
extern const int g_funcs_count;

/* ---------------- erros ---------------- */
void rt_backtrace(Cpu *c) {
    fprintf(stderr, "  pilha de retornos (mais recente primeiro):\n");
    for (int i = c->rsp - 1, n = 0; i >= 0 && n < 16; i--, n++)
        fprintf(stderr, "    volta para %08x\n", c->rs[i]);
}
__attribute__((noreturn)) void rt_fatal(Cpu *c, const char *msg, uint32_t a) {
    fprintf(stderr, "\n[recomp] ERRO: %s %08x (quadro %llu)\n", msg, a, (unsigned long long)g_frame);
    for (int i = 0; i < 16; i += 4)
        fprintf(stderr, "  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n", i, c->r[i], i + 1, c->r[i + 1], i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    rt_backtrace(c);
    exit(1);
}
void rt_unimpl(Cpu *c, uint32_t addr, uint32_t insn) {
    fprintf(stderr, "[recomp] instrução não suportada %08x em %08x\n", insn, addr);
    rt_fatal(c, "parou em", addr);
}

/* ---------------- cartão (ROM) ---------------- */
uint8_t *g_rom; size_t g_rom_size;
static uint8_t g_card_cmd[8];
static uint8_t g_card_buf[0x4000];
static uint32_t g_card_len, g_card_pos;
void rt_check_irq(Cpu *c);
static void card_finish(Cpu *c);

/* ---------------- I/O ---------------- */
static uint32_t io_rd16(Cpu *c, uint32_t off);
static void io_wr16(Cpu *c, uint32_t off, uint32_t v);
static uint32_t io_get32(uint32_t off) { uint32_t v; memcpy(&v, g_io + off, 4); return v; }
static void io_set32(uint32_t off, uint32_t v) { memcpy(g_io + off, &v, 4); }
static uint16_t io_get16(uint32_t off) { uint16_t v; memcpy(&v, g_io + off, 2); return v; }
static void io_set16(uint32_t off, uint16_t v) { memcpy(g_io + off, &v, 2); }

static void math_div(void) {
    uint32_t cnt = io_get16(0x280) & 3;
    int64_t num, den;
    if (cnt == 0) { num = (int32_t)io_get32(0x290); den = (int32_t)io_get32(0x298); }
    else if (cnt == 1) { memcpy(&num, g_io + 0x290, 8); den = (int32_t)io_get32(0x298); }
    else { memcpy(&num, g_io + 0x290, 8); memcpy(&den, g_io + 0x298, 8); }
    int64_t q, r;
    if (den == 0) { q = num < 0 ? 1 : -1; r = num; io_set16(0x280, io_get16(0x280) | 0x4000); }
    else if (num == INT64_MIN && den == -1) { q = num; r = 0; }
    else { q = num / den; r = num % den; io_set16(0x280, io_get16(0x280) & ~0x4000); }
    memcpy(g_io + 0x2A0, &q, 8); memcpy(g_io + 0x2A8, &r, 8);
}
static void math_sqrt(void) {
    uint64_t v;
    if (io_get16(0x2B0) & 1) memcpy(&v, g_io + 0x2B8, 8); else v = io_get32(0x2B8);
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    io_set32(0x2B4, (uint32_t)r);
}
static uint32_t card_next_word(Cpu *c) {
    uint32_t v = 0;
    if (g_card_pos < g_card_len) { memcpy(&v, g_card_buf + g_card_pos, 4); g_card_pos += 4; }
    if (g_card_pos >= g_card_len) card_finish(c);
    return v;
}
static void card_start(Cpu *c, uint32_t ctrl) {
    uint32_t bs = (ctrl >> 24) & 7;
    g_card_len = bs == 0 ? 0 : bs == 7 ? 4 : (0x100u << bs);
    g_card_pos = 0;
    memset(g_card_buf, 0xFF, g_card_len);
    uint8_t cmd = g_card_cmd[0];
    if (getenv("RECOMP_CARDLOG")) fprintf(stderr, "[card] cmd %02x %02x%02x%02x%02x len %x\n", cmd, g_card_cmd[1], g_card_cmd[2], g_card_cmd[3], g_card_cmd[4], g_card_len);
    if (cmd == 0xB7) {
        uint32_t a = (g_card_cmd[1] << 24) | (g_card_cmd[2] << 16) | (g_card_cmd[3] << 8) | g_card_cmd[4];
        if (a < 0x8000) a = 0x8000 + (a & 0x1FF);
        for (uint32_t i = 0; i < g_card_len; i++) { uint32_t p = a + i; g_card_buf[i] = p < g_rom_size ? g_rom[p] : 0xFF; }
    } else if (cmd == 0xB8) {
        uint32_t id = 0x00001FC2; /* fabricante C2, 512 MB */
        for (uint32_t i = 0; i < g_card_len; i += 4) memcpy(g_card_buf + i, &id, 4);
    } else if (cmd == 0xD6) {
        memset(g_card_buf, 0, g_card_len);
    } else {
        fprintf(stderr, "[recomp] comando de cartão desconhecido %02x\n", cmd);
    }
    if (g_card_len == 0) { card_finish(c); return; }
    io_set32(0x1A4, ctrl | 0x80800000u); /* ocupado + palavra pronta */
    /* DMA no modo cartão (timing 5): transfere tudo de uma vez */
    for (int ch = 0; ch < 4; ch++) {
        uint32_t base = 0xB0 + ch * 12, cnt = io_get32(base + 8);
        if ((cnt >> 31) && ((cnt >> 27) & 7) == 5) {
            uint32_t dst = io_get32(base + 4);
            int dm = (cnt >> 21) & 3;
            while (g_card_pos < g_card_len) { wr32(c, dst, card_next_word(c)); if (dm == 0 || dm == 3) dst += 4; else if (dm == 1) dst -= 4; }
            if (!((cnt >> 25) & 1)) io_set32(base + 8, cnt & ~0x80000000u); /* sem repeat: desliga */
            if ((cnt >> 30) & 1) { io_set32(0x214, io_get32(0x214) | (1u << (8 + ch))); }
            rt_check_irq(c);
            break;
        }
    }
}
static void card_finish(Cpu *c) {
    io_set32(0x1A4, io_get32(0x1A4) & ~0x80800000u);
    if (io_get16(0x1A0) & 0x4000) io_set32(0x214, io_get32(0x214) | (1u << 19));
    rt_check_irq(c);
}

static void dma_run(Cpu *c, int ch) {
    uint32_t base = 0xB0 + ch * 12;
    uint32_t cnt = io_get32(base + 8);
    if (!(cnt >> 31)) return;
    uint32_t timing = (cnt >> 27) & 7;
    if (timing != 0) return; /* só DMA imediato por enquanto */
    uint32_t src = io_get32(base), dst = io_get32(base + 4);
    uint32_t n = cnt & 0x1FFFFF; if (!n) n = 0x200000;
    int w32 = (cnt >> 26) & 1, sz = w32 ? 4 : 2;
    int sm = (cnt >> 23) & 3, dm = (cnt >> 21) & 3;
    for (uint32_t i = 0; i < n; i++) {
        if (w32) wr32(c, dst, rd32(c, src)); else wr16(c, dst, rd16(c, src));
        if (sm == 0) src += sz; else if (sm == 1) src -= sz;
        if (dm == 0 || dm == 3) dst += sz; else if (dm == 1) dst -= sz;
    }
    io_set32(base + 8, cnt & ~0x80000000u);
    if ((cnt >> 30) & 1) { io_set32(0x214, io_get32(0x214) | (1u << (8 + ch))); rt_check_irq(c); }
}

extern uint32_t g_keys; extern int g_touch, g_touch_x, g_touch_y;
/* ---------------- PXI (FIFO ARM9 <-> ARM7) e ARM7 em HLE ---------------- */
#define SND_SHARED_WORK_PTR 0x02095920u  /* variável com o ponteiro do SNDi_SharedWork (literal em 0200c3e4) */
static uint32_t g_fifo_rx[16]; static int g_fifo_rx_n;
static void fifo_push_rx(Cpu *c, uint32_t w) {
    if (g_fifo_rx_n < 16) g_fifo_rx[g_fifo_rx_n++] = w;
    if (io_get16(0x184) & 0x400) { io_set32(0x214, io_get32(0x214) | (1u << 18)); rt_check_irq(c); }
}
static uint32_t fifo_pop_rx(void) {
    if (!g_fifo_rx_n) return 0;
    uint32_t w = g_fifo_rx[0];
    memmove(g_fifo_rx, g_fifo_rx + 1, --g_fifo_rx_n * 4);
    return w;
}
/* Backup (save) do cartão: o ARM9 manda pedidos pelo PXI (tag 11) e o ARM7 faz o acesso SPI.
 * Aqui o save é um arquivo comum. Layout do CARDiCommandArg: +0 result, +4 type, +8 id, +C src, +10 dst, +14 len. */
static uint8_t g_save[1 << 20]; static int g_save_loaded; static uint32_t g_card_cmdarg; static int g_card_wait_ptr;
static const char *save_path(void) { const char *p = getenv("RECOMP_SAVE"); return p ? p : "ie3.sav"; }
static void save_load(void) {
    if (g_save_loaded) return;
    g_save_loaded = 1; memset(g_save, 0xFF, sizeof g_save);
    FILE *f = fopen(save_path(), "rb"); if (f) { size_t n = fread(g_save, 1, sizeof g_save, f); (void)n; fclose(f); }
}
static void save_flush(void) { FILE *f = fopen(save_path(), "wb"); if (f) { fwrite(g_save, 1, sizeof g_save, f); fclose(f); } }
static void card_backup_request(Cpu *c, uint32_t req) {
    save_load();
    uint32_t a = g_card_cmdarg;
    uint32_t src = rd32(c, a + 0xC), dst = rd32(c, a + 0x10), len = rd32(c, a + 0x14);
    if (getenv("RECOMP_SAVELOG")) fprintf(stderr, "[save] pedido %u src=%08x dst=%08x len=%x\n", req, src, dst, len);
    switch (req) {
    case 6: /* READ_BACKUP: src = endereço no save */
        for (uint32_t i = 0; i < len; i++) wr8(c, dst + i, g_save[(src + i) & (sizeof g_save - 1)]);
        break;
    case 7: case 8: /* WRITE / PROGRAM: dst = endereço no save */
        for (uint32_t i = 0; i < len; i++) g_save[(dst + i) & (sizeof g_save - 1)] = rd8(c, src + i);
        save_flush();
        break;
    case 9: { /* VERIFY: compara */
        int ok = 1;
        for (uint32_t i = 0; i < len; i++) if (g_save[(dst + i) & (sizeof g_save - 1)] != rd8(c, src + i)) ok = 0;
        if (!ok) { wr32(c, a, 2); goto reply; }
        break; }
    case 10: case 11: case 12: case 13: /* apagar: vira 0xFF */
        for (uint32_t i = 0; i < len; i++) g_save[(dst + i) & (sizeof g_save - 1)] = 0xFF;
        save_flush();
        break;
    }
    wr32(c, a, 0); /* CARD_RESULT_SUCCESS */
reply:
    fifo_push_rx(c, (req << 6) | (1u << 5) | 11);
}

static void arm7_hle_receive(Cpu *c, uint32_t w) {
    uint32_t tag = w & 0x1F, data = w >> 6;
    static int seen[32];
    if (getenv("RECOMP_PXILOG") || !seen[tag]++) fprintf(stderr, "[pxi] ARM9 -> ARM7 tag %u dado %06x\n", tag, data);
    switch (tag) {
    case 6: { /* tela de toque: amostra em 02FFFFAA (x 0-11, y 12-23, bit 24 tocando) e responde */
        uint32_t cmd = (data >> 8) & 0x7F;
        /* x 0-11 e y 12-23 em valores brutos do ADC, coerentes com a calibração gravada em main.c */
        uint32_t tp = g_touch ? ((uint32_t)(g_touch_x * 16) & 0xFFF) | (((uint32_t)(g_touch_y * 12 + 0x80) & 0xFFF) << 12) | (1u << 24) : 0;
        uint16_t lo = tp & 0xFFFF, hi = tp >> 16;
        wr16(c, 0x02FFFFAA, lo); wr16(c, 0x02FFFFAC, hi);
        if (data & 0x1000000) fifo_push_rx(c, ((0x1000000u | (cmd << 8)) << 6) | 6);
        break; }
    case 11: /* cartão (backup) */
        if (g_card_wait_ptr) { g_card_cmdarg = data; g_card_wait_ptr = 0; card_backup_request(c, 0); break; } /* 2º envio do INIT: ponteiro */
        if (data == 0) { g_card_wait_ptr = 1; break; }
        card_backup_request(c, data);
        break;
    case 7: { /* som: o ARM7 "processou" a lista de comandos */
        uint32_t sw = rd32(c, SND_SHARED_WORK_PTR);
        if (sw) wr32(c, sw, rd32(c, sw) + 1);
        break; }
    }
    (void)fifo_push_rx;
}

/* ---------------- entrada ---------------- */
/* g_keys: bits pressionados (0 A,1 B,2 SELECT,3 START,4 DIREITA,5 ESQUERDA,6 CIMA,7 BAIXO,8 R,9 L,10 X,11 Y) */
uint32_t g_keys; int g_touch, g_touch_x, g_touch_y;
/* Roteiro de entrada para testes sem janela: RECOMP_INPUT="100-110:A;300-305:TOUCH=128,96;..." */
static void input_script(void) {
    const char *s = getenv("RECOMP_INPUT");
    if (!s) return;
    static const char *names[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
    uint32_t keys = 0; int touch = 0, tx = 0, ty = 0;
    const char *p = s;
    while (*p) {
        unsigned long a = strtoul(p, (char **)&p, 10), b = a;
        if (*p == '-') b = strtoul(p + 1, (char **)&p, 10);
        if (*p == ':') p++;
        const char *end = strchr(p, ';'); size_t len = end ? (size_t)(end - p) : strlen(p);
        if (g_frame >= a && g_frame <= b) {
            char item[64]; snprintf(item, sizeof item, "%.*s", (int)(len < 63 ? len : 63), p);
            for (char *tok = strtok(item, "+"); tok; tok = strtok(NULL, "+")) {
                if (!strncmp(tok, "TOUCH=", 6)) { touch = 1; sscanf(tok + 6, "%d,%d", &tx, &ty); continue; }
                for (int k = 0; k < 12; k++) if (!strcmp(tok, names[k])) keys |= 1u << k;
            }
        }
        p += len; if (*p == ';') p++;
    }
    g_keys = keys; g_touch = touch; g_touch_x = tx; g_touch_y = ty;
}
static void input_update(Cpu *c) {
    input_script();
    /* o ARM7 copia X/Y, caneta e dobradiça para 02FFFFA8 */
    uint16_t xy = 0x7F & ~((g_keys >> 10) & 3);
    if (g_touch) xy &= ~0x40;
    xy &= ~0x80; /* tela aberta */
    wr16(c, 0x02FFFFA8, xy);
}

/* ---------------- timers ---------------- */
/* O tempo só anda no VBlank (e no halt): cada quadro = 560190 ciclos do barramento de 33,5 MHz. */
static uint32_t g_tm_reload[4], g_tm_count[4], g_tm_frac[4];
static void timers_advance(uint32_t cycles) {
    static const int pre[4] = {0, 6, 8, 10};
    uint32_t overflow_prev = 0;
    for (int t = 0; t < 4; t++) {
        uint16_t ctl = io_get16(0x102 + t * 4);
        uint32_t ovf = 0;
        if (ctl & 0x80) {
            uint64_t ticks;
            if (t > 0 && (ctl & 4)) ticks = overflow_prev;
            else { uint64_t total = (uint64_t)cycles + g_tm_frac[t]; ticks = total >> pre[ctl & 3]; g_tm_frac[t] = total & ((1u << pre[ctl & 3]) - 1); }
            uint64_t v = g_tm_count[t] + ticks;
            while (v > 0xFFFF) { ovf++; v = v - 0x10000 + g_tm_reload[t]; if (g_tm_reload[t] == 0xFFFF) { v = 0xFFFF; break; } }
            g_tm_count[t] = (uint32_t)v;
            if (ovf && (ctl & 0x40)) io_set32(0x214, io_get32(0x214) | (1u << (3 + t)));
        }
        overflow_prev = ovf;
    }
}

static uint32_t io_rd16(Cpu *c, uint32_t off) {
    (void)c;
    switch (off) {
    case 0x004: { uint16_t v = io_get16(4) & ~7; if (g_vcount >= 192 && g_vcount < 262) v |= 1; return v; }
    case 0x006: g_vcount = (g_vcount + 1) % 263; return g_vcount;
    case 0x130: return 0x3FF & ~g_keys;
    case 0x136: return 0x007F;       /* X/Y soltos, tela aberta */
    case 0x184: return (io_get16(0x184) & 0x8404) | 0x0001 | (g_fifo_rx_n ? 0 : 0x100); /* envio sempre vazio */
    case 0x214: case 0x216: return io_get16(off);
    case 0x100: case 0x104: case 0x108: case 0x10C: return g_tm_count[(off - 0x100) / 4];
    }
    return io_get16(off);
}
static void io_wr16(Cpu *c, uint32_t off, uint32_t v) {
    if (off == 0x214 || off == 0x216) { io_set16(off, io_get16(off) & ~v); return; } /* IF: 1 limpa */
    if (off == 0x180) { /* IPCSYNC: o ARM7 (HLE) responde ao valor do ARM9 com valor+1 (handshake do PXI_Init) */
        uint32_t out = (v >> 8) & 0xF;
        io_set16(0x180, (uint16_t)((v & 0x4F00) | ((out + 1) & 0xF)));
        return;
    }
    if (off >= 0x100 && off < 0x110) {
        int t = (off - 0x100) / 4;
        if (off & 2) { if ((v & 0x80) && !(io_get16(off) & 0x80)) { g_tm_count[t] = g_tm_reload[t]; g_tm_frac[t] = 0; } }
        else { g_tm_reload[t] = v & 0xFFFF; return; }
    }
    io_set16(off, (uint16_t)v);
    if (off >= 0x240 && off < 0x24A) video_remap(g_io);
    if (off == 0x1A6 && (v & 0x8000)) card_start(c, io_get32(0x1A4));
    if (off == 0x18A) arm7_hle_receive(c, io_get32(0x188)); /* escrita de 32 bits em IPCFIFOSEND termina aqui */
    if (off >= 0x1A8 && off < 0x1B0) { g_card_cmd[off - 0x1A8] = v & 0xFF; g_card_cmd[off - 0x1A8 + 1] = (v >> 8) & 0xFF; }
    if (off == 0x208 || off == 0x210 || off == 0x212) rt_check_irq(c);
    if (off >= 0x280 && off < 0x2A0) math_div();
    if (off >= 0x2B0 && off < 0x2C0) math_sqrt();
    if (off >= 0xB0 && off < 0xE0 && ((off - 0xB0) % 12) == 10) dma_run(c, (off - 0xB0) / 12);
}

/* ---------------- memória (caminho lento) ---------------- */
static uint8_t *mem_ptr(uint32_t a, int write) {
    (void)write;
    if (a - DTCM_BASE < 0x4000u) return g_dtcm + (a - DTCM_BASE);
    switch (a >> 24) {
    case 0x00: case 0x01: return g_itcm + (a & 0x7FFF);
    case 0x02: return g_main + (a & 0x3FFFFF);
    case 0x03: return g_wram + (a & 0x7FFF);
    case 0x05: return g_pal + (a & 0x7FF);
    case 0x06: return video_vram_ptr(a);
    case 0x07: return g_oam + (a & 0x7FF);
    }
    return NULL;
}
static int is_io(uint32_t a) { return (a >> 24) == 4; }

uint32_t mem_rd32_slow(Cpu *c, uint32_t a) {
    if (is_io(a)) {
        uint32_t off = a & 0xFFFFFF;
        if (off == 0x100000) return fifo_pop_rx(); /* IPC FIFO RECV */
        if (off == 0x100010) return card_next_word(c);
        if (off < 0x2000) return io_rd16(c, off) | (io_rd16(c, off + 2) << 16);
        return 0;
    }
    uint8_t *p = mem_ptr(a, 0);
    if (!p) return (a >> 24) == 8 || (a >> 24) == 9 ? 0xFFFFFFFF : 0;
    uint32_t v; memcpy(&v, p, 4); return v;
}
uint32_t mem_rd16_slow(Cpu *c, uint32_t a) {
    if (is_io(a)) { uint32_t off = a & 0xFFFFFF; return off < 0x2000 ? io_rd16(c, off) : 0; }
    uint8_t *p = mem_ptr(a, 0);
    if (!p) return (a >> 24) == 8 || (a >> 24) == 9 ? 0xFFFF : 0;
    uint16_t v; memcpy(&v, p, 2); return v;
}
uint32_t mem_rd8_slow(Cpu *c, uint32_t a) {
    if (is_io(a)) { uint32_t off = a & 0xFFFFFF; return off < 0x2000 ? (io_rd16(c, off & ~1) >> ((off & 1) * 8)) & 0xFF : 0; }
    uint8_t *p = mem_ptr(a, 0);
    if (!p) return (a >> 24) == 8 || (a >> 24) == 9 ? 0xFF : 0;
    return *p;
}
void mem_wr32_slow(Cpu *c, uint32_t a, uint32_t v) {
    if (is_io(a)) {
        uint32_t off = a & 0xFFFFFF;
        if (off < 0x2000) { io_wr16(c, off, v & 0xFFFF); io_wr16(c, off + 2, v >> 16); }
        return;
    }
    uint8_t *p = mem_ptr(a, 1);
    if (p) memcpy(p, &v, 4);
}
void mem_wr16_slow(Cpu *c, uint32_t a, uint32_t v) {
    if (is_io(a)) { uint32_t off = a & 0xFFFFFF; if (off < 0x2000) io_wr16(c, off, v); return; }
    uint8_t *p = mem_ptr(a, 1);
    if (p) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }
}
void mem_wr8_slow(Cpu *c, uint32_t a, uint32_t v) {
    if (is_io(a)) {
        uint32_t off = a & 0xFFFFFF;
        if (off < 0x2000) { uint32_t h = io_get16(off & ~1); h = (off & 1) ? (h & 0xFF) | (v << 8) : (h & 0xFF00) | (v & 0xFF); io_wr16(c, off & ~1, h); }
        return;
    }
    uint8_t *p = mem_ptr(a, 1);
    if (p && (a >> 24) != 6 && (a >> 24) != 5 && (a >> 24) != 7) *p = (uint8_t)v; /* VRAM/paleta/OAM ignoram escrita de 8 bits */
}

/* ---------------- CPSR / modos ---------------- */
static int mode_idx(uint32_t m) { return m & 0x1F; }
static void switch_mode(Cpu *c, uint32_t newm) {
    uint32_t old = c->mode;
    if (old == newm) return;
    int oi = (old == 0x1F) ? 0x10 : mode_idx(old), ni = (newm == 0x1F) ? 0x10 : mode_idx(newm);
    if (oi != ni) {
        c->bank_sp[oi] = c->r[13]; c->bank_lr[oi] = c->r[14];
        c->r[13] = c->bank_sp[ni]; c->r[14] = c->bank_lr[ni];
    }
    c->mode = newm;
}
uint32_t rt_get_cpsr(Cpu *c) {
    return (c->N << 31) | (c->Z << 30) | (c->C << 29) | (c->V << 28) | (c->Q << 27) | (c->I << 7) | (c->F << 6) | c->mode;
}
uint32_t rt_get_spsr(Cpu *c) { return c->spsr[mode_idx(c->mode)]; }
void rt_set_cpsr(Cpu *c, uint32_t v, int mask) {
    if (mask & 8) { c->N = v >> 31; c->Z = (v >> 30) & 1; c->C = (v >> 29) & 1; c->V = (v >> 28) & 1; c->Q = (v >> 27) & 1; }
    if (mask & 1) { uint32_t was = c->I; c->I = (v >> 7) & 1; c->F = (v >> 6) & 1; switch_mode(c, v & 0x1F); if (was && !c->I) rt_check_irq(c); }
}
void rt_set_spsr(Cpu *c, uint32_t v, int mask) {
    uint32_t *s = &c->spsr[mode_idx(c->mode)], m = 0;
    if (mask & 1) m |= 0xFF; if (mask & 2) m |= 0xFF00; if (mask & 4) m |= 0xFF0000; if (mask & 8) m |= 0xFF000000;
    *s = (*s & ~m) | (v & m);
}
void rt_exception_return(Cpu *c, uint32_t target) { rt_fatal(c, "retorno de exceção (movs pc) não suportado, alvo", target); }

/* ---------------- CP15 ---------------- */
uint32_t rt_mrc(Cpu *c, int cp, int op1, int crn, int crm, int op2) {
    (void)c; (void)op1; (void)crm;
    if (cp != 15) return 0;
    if (crn == 0) return op2 == 1 ? 0x0F0D2112 : 0x41059461;
    if (crn == 1) return 0x0005707D;
    if (crn == 9 && crm == 1) return op2 == 0 ? (DTCM_BASE | 0x0A) : 0x0000000C;
    return 0;
}
void rt_vblank(Cpu *c);
void rt_mcr(Cpu *c, int cp, int op1, int crn, int crm, int op2, uint32_t v) {
    (void)op1; (void)v;
    if (cp == 15 && crn == 7 && crm == 0 && op2 == 4) rt_vblank(c); /* wait for interrupt: avança até o próximo VBlank */
    /* caches/MPU: nada a fazer */
}

/* ---------------- SWI (BIOS em HLE) ---------------- */
void rt_swi(Cpu *c, uint32_t n) {
    switch (n) {
    case 0x03: return; /* WaitByLoop */
    case 0x06: rt_vblank(c); return; /* Halt: avança até o próximo VBlank */
    case 0x09: { int32_t a = c->r[0], b = c->r[1]; if (!b) rt_fatal(c, "divisão por zero (SWI 9)", 0);
                 c->r[0] = a / b; c->r[1] = a % b; c->r[3] = (uint32_t)abs(a / b); return; }
    case 0x0B: { /* CpuSet */
        uint32_t src = c->r[0], dst = c->r[1], cnt = c->r[2], n2 = cnt & 0x1FFFFF;
        int fill = (cnt >> 24) & 1, w32 = (cnt >> 26) & 1;
        uint32_t v = w32 ? rd32(c, src) : rd16(c, src);
        for (uint32_t i = 0; i < n2; i++) {
            if (!fill) v = w32 ? rd32(c, src + i * 4) : rd16(c, src + i * 2);
            if (w32) wr32(c, dst + i * 4, v); else wr16(c, dst + i * 2, v);
        }
        return; }
    case 0x0C: { /* CpuFastSet */
        uint32_t src = c->r[0], dst = c->r[1], cnt = c->r[2], n2 = (cnt & 0x1FFFFF + 7) & ~7u;
        n2 = ((cnt & 0x1FFFFF) + 7) & ~7u;
        int fill = (cnt >> 24) & 1; uint32_t v = rd32(c, src);
        for (uint32_t i = 0; i < n2; i++) { if (!fill) v = rd32(c, src + i * 4); wr32(c, dst + i * 4, v); }
        return; }
    case 0x0D: { uint32_t v = c->r[0], r = 0, bit = 1u << 30; while (bit > v) bit >>= 2;
                 while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
                 c->r[0] = r; return; }
    case 0x11: case 0x12: { /* LZ77 */
        uint32_t src = c->r[0], dst = c->r[1], hdr = rd32(c, src); src += 4;
        uint32_t size = hdr >> 8, out = 0;
        while (out < size) {
            uint32_t flags = rd8(c, src++);
            for (int i = 0; i < 8 && out < size; i++, flags <<= 1) {
                if (flags & 0x80) {
                    uint32_t b1 = rd8(c, src++), b2 = rd8(c, src++);
                    uint32_t len = (b1 >> 4) + 3, disp = ((b1 & 15) << 8 | b2) + 1;
                    for (uint32_t k = 0; k < len && out < size; k++, out++) wr8(c, dst + out, rd8(c, dst + out - disp));
                } else { wr8(c, dst + out, rd8(c, src++)); out++; }
            }
        }
        return; }
    }
    rt_fatal(c, "SWI não implementado", n);
}

/* ---------------- despacho ---------------- */
#define HT_SIZE (1 << 16)
static struct { uint32_t key; int module; GenFunc fn; } *g_ht;
static int g_loaded[NUM_OVERLAYS];

static uint32_t hkey(int module, uint32_t a) { return (a * 2654435761u) ^ (uint32_t)(module * 40503); }
static void ht_insert(int module, uint32_t a, GenFunc fn) {
    uint32_t h = hkey(module, a) & (HT_SIZE - 1);
    while (g_ht[h].fn) h = (h + 1) & (HT_SIZE - 1);
    g_ht[h].key = a; g_ht[h].module = module; g_ht[h].fn = fn;
}
static GenFunc ht_find(int module, uint32_t a) {
    uint32_t h = hkey(module, a) & (HT_SIZE - 1);
    while (g_ht[h].fn) { if (g_ht[h].key == a && g_ht[h].module == module) return g_ht[h].fn; h = (h + 1) & (HT_SIZE - 1); }
    return NULL;
}
static uint8_t *g_ov_image[NUM_OVERLAYS];
static size_t g_ov_image_size[NUM_OVERLAYS];
static const uint8_t *ov_image(int id, size_t *sz);
static void ov_mark_loaded(int id);
static GenFunc lookup(uint32_t a) {
    GenFunc f = ht_find(-1, a); if (f) return f;
    f = ht_find(-2, a); if (f) return f;
    for (int i = 0; i < NUM_OVERLAYS; i++)
        if (g_loaded[i] && a >= g_overlays[i].ram && a < g_overlays[i].ram + g_overlays[i].size)
            if ((f = ht_find(i, a))) return f;
    /* overlay carregado por outro caminho (ex.: leitura assíncrona): reconhece pelo conteúdo da memória */
    if (a >= 0x020bcb40 && a < 0x021604c0) {
        for (int i = 0; i < NUM_OVERLAYS; i++) {
            if (a < g_overlays[i].ram || a + 64 > g_overlays[i].ram + g_overlays[i].size) continue;
            size_t sz; const uint8_t *img = ov_image(i, &sz);
            uint32_t off = a - g_overlays[i].ram;
            if (!memcmp(img + off, g_main + (a & 0x3FFFFF), 64) && (f = ht_find(i, a))) {
                fprintf(stderr, "[recomp] overlay %d reconhecido pelo conteúdo da memória\n", i);
                ov_mark_loaded(i);
                return f;
            }
        }
    }
    return NULL;
}
void rt_call(Cpu *c, uint32_t target) {
    uint32_t ret = c->r[14];
    if (c->rsp >= 4000) rt_fatal(c, "pilha de retornos estourou em", target);
    c->rs[c->rsp++] = ret;
    if (target & 1) { thumb_exec(c, target & ~1u); c->rsp--; return; }
    GenFunc f = lookup(target);
    if (!f) {
        /* registra o endereço para a próxima geração (recomp/armrecomp.py lê analysis/extra_functions.txt) */
        FILE *x = fopen("analysis/extra_functions.txt", "a");
        if (x) {
            const char *mod = "arm9";
            char buf[16];
            if (target < 0x02000000) mod = "itcm";
            else if (target >= 0x020bcb40 && target < 0x021604c0) {
                for (int i = 0; i < NUM_OVERLAYS; i++) if (g_loaded[i] && target >= g_overlays[i].ram && target < g_overlays[i].ram + g_overlays[i].size) { snprintf(buf, sizeof buf, "ov%03d", i); mod = buf; }
            }
            fprintf(x, "%s %08x arm\n", mod, target); fclose(x);
            fprintf(stderr, "[recomp] endereço anotado em analysis/extra_functions.txt; rode 'make gen' e compile de novo\n");
        }
        rt_fatal(c, "nenhuma função conhecida em", target);
    }
    if (g_trace) fprintf(stderr, "[call] %08x\n", target);
    f(c);
    c->rsp--;
}
void rt_jump(Cpu *c, uint32_t target) {
    if (c->rsp > 0 && (target & ~1u) == (c->rs[c->rsp - 1] & ~1u)) return; /* é um retorno */
    /* chamada em cauda: o alvo volta para o mesmo lugar que nós */
    uint32_t saved = c->r[14];
    c->r[14] = c->rsp > 0 ? c->rs[c->rsp - 1] : 0;
    rt_call(c, target);
    (void)saved;
}
void rt_fallthrough(Cpu *c, uint32_t next) { rt_jump(c, next); }

/* ---------------- overlays (HLE de FS_LoadOverlay) ---------------- */
extern uint8_t *load_file(const char *path, size_t *size);
extern const char *g_extracted;
static const uint8_t *ov_image(int id, size_t *sz) {
    if (!g_ov_image[id]) {
        char path[512]; snprintf(path, sizeof path, "%s/ov/ov%03d.bin", g_extracted, id);
        g_ov_image[id] = load_file(path, &g_ov_image_size[id]);
    }
    *sz = g_ov_image_size[id];
    return g_ov_image[id];
}
static void ov_mark_loaded(int id) {
    const OverlayInfo *o = &g_overlays[id];
    for (int i = 0; i < NUM_OVERLAYS; i++) /* descarrega quem ocupa a mesma memória */
        if (g_loaded[i] && i != id && o->ram < g_overlays[i].ram + g_overlays[i].size + g_overlays[i].bss &&
            g_overlays[i].ram < o->ram + o->size + o->bss) g_loaded[i] = 0;
    g_loaded[id] = 1;
}
/* copia a imagem descomprimida, zera o bss, roda os static init e marca como carregado */
static void ov_start(Cpu *c, uint32_t id) {
    if (id >= NUM_OVERLAYS) rt_fatal(c, "overlay inválido", id);
    size_t sz; const uint8_t *d = ov_image(id, &sz);
    const OverlayInfo *o = &g_overlays[id];
    for (size_t i = 0; i < sz; i++) wr8(c, o->ram + i, d[i]);
    for (uint32_t i = 0; i < o->bss; i++) wr8(c, o->ram + o->size + i, 0);
    ov_mark_loaded(id);
    fprintf(stderr, "[recomp] overlay %u carregado em %08x (quadro %llu)\n", id, o->ram, (unsigned long long)g_frame);
    uint32_t saved[16]; memcpy(saved, c->r, sizeof saved);
    for (uint32_t p = o->sinit_start; p < o->sinit_end; p += 4) {
        uint32_t fn = rd32(c, p);
        if (fn) { c->r[14] = 0xFFFF0000u; rt_call(c, fn); }
    }
    memcpy(c->r, saved, sizeof saved);
}
void hle_FS_LoadOverlay(Cpu *c) { ov_start(c, c->r[1]); c->r[0] = 1; }
/* FS_StartOverlay(FSOverlayInfo *p): usado depois de FS_LoadOverlayImage(Async) */
void hle_FS_StartOverlay(Cpu *c) { ov_start(c, rd32(c, c->r[0])); }
void hle_FS_UnloadOverlay(Cpu *c) {
    uint32_t id = c->r[1];
    if (id < NUM_OVERLAYS) g_loaded[id] = 0;
    fprintf(stderr, "[recomp] overlay %u descarregado\n", id);
    c->r[0] = 1;
}

/* ---------------- IRQ / VBlank ---------------- */
extern void video_present(void);
static int g_in_irq;
void rt_after_irq(Cpu *c);
/* entrega IRQs pendentes se IME=1 e CPSR.I=0 (como o hardware faria) */
void rt_check_irq(Cpu *c) {
    if (g_in_irq || c->I || !(io_get32(0x208) & 1)) return;
    uint32_t pend = io_get32(0x210) & io_get32(0x214);
    if (!pend) return;
    g_in_irq = 1;
    Cpu save = *c;
    switch_mode(c, 0x12); /* handlers rodam em modo IRQ, na pilha de IRQ: o reschedule fica adiado */
    for (int b = 0; b < 32; b++) {
        if (!(pend >> b & 1)) continue;
        io_set32(0x214, io_get32(0x214) & ~(1u << b));
        uint32_t h = rd32(c, DTCM_BASE + b * 4);
        static int logged[32];
        if (getenv("RECOMP_IRQLOG") && logged[b]++ < 3) fprintf(stderr, "[irq] bit %d handler %08x quadro %llu\n", b, h, (unsigned long long)g_frame);
        if (!h) continue;
        c->I = 1;
        c->r[14] = 0xFFFF0018u;
        rt_call(c, h);
    }
    int rsp = c->rsp;
    uint32_t irq_sp = c->r[13];
    *c = save; c->rsp = rsp;
    c->bank_sp[0x12] = irq_sp;
    g_in_irq = 0;
    rt_after_irq(c);
}
static void raise_irq(Cpu *c, uint32_t bits) {
    io_set32(0x214, io_get32(0x214) | bits);
    rt_check_irq(c);
}
void rt_after_irq(Cpu *c);
void rt_vblank(Cpu *c) {
    g_frame++;
    g_vcount = 192;
    timers_advance(560190);
    input_update(c);
    io_set32(0x214, io_get32(0x214) | 1);
    { uint32_t I = c->I; c->I = 0; rt_check_irq(c); c->I = I; } /* halt acorda com IRQ mesmo com I=1 */
    video_present();
    if (g_frame % 60 == 0) fprintf(stderr, "[recomp] quadro %llu\n", (unsigned long long)g_frame);
}
/* OS_WaitIrq(BOOL clear, u32 mask) */
void hle_OS_WaitIrq(Cpu *c) {
    uint32_t clear = c->r[0], mask = c->r[1];
    uint32_t chk = DTCM_BASE + 0x3FF8;
    if (clear) wr32(c, chk, rd32(c, chk) & ~mask);
    for (int tries = 0; tries < 600; tries++) {
        if (rd32(c, chk) & mask) { wr32(c, chk, rd32(c, chk) & ~mask); return; }
        if (mask & 1) rt_vblank(c);
        else rt_fatal(c, "OS_WaitIrq esperando IRQ que não é VBlank, máscara", mask);
    }
    rt_fatal(c, "OS_WaitIrq: a IRQ nunca chegou, máscara", mask);
}
void rt_init_dispatch(void) {
    g_ht = calloc(HT_SIZE, sizeof *g_ht);
    for (int i = 0; i < g_funcs_count; i++) ht_insert(g_funcs[i].module, g_funcs[i].addr, g_funcs[i].fn);
}
