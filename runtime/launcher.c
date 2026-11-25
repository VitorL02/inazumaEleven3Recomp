#include <stdio.h>
#include <string.h>
#include "launcher.h"
#include "config.h"
#include "rom.h"
#include "font.h"

#define EXPECTED_SHA1 "a70e9a336f6ea8ff78003938dcd024533f034a68"

static int g_sel;
static char g_status[256];
static int g_rom_ok;

enum { ITEM_PLAY, ITEM_FPS, ITEM_SCALE, ITEM_FULL, ITEM_LAYOUT, ITEM_SMOOTH, ITEM_VSYNC, ITEM_QUIT, ITEM_COUNT };
static const int fps_opts[] = {30, 60, 120, 144, 0};

/* valida só o essencial sem carregar a ROM inteira de novo se já foi validada */
static void check_rom(void) {
    if (!g_cfg.rom[0]) { g_rom_ok = 0; snprintf(g_status, sizeof g_status, "Arraste o arquivo .nds do jogo para esta janela."); return; }
    char err[256];
    if (rom_open(g_cfg.rom, err, sizeof err)) { g_rom_ok = 0; snprintf(g_status, sizeof g_status, "ROM inválida: %s", err); return; }
    if (strcmp(g_romfile.sha1, EXPECTED_SHA1)) {
        g_rom_ok = 0;
        snprintf(g_status, sizeof g_status, "Versão diferente da esperada (tradução BR T1.3). SHA-1: %.12s...", g_romfile.sha1);
        return;
    }
    g_rom_ok = 1; snprintf(g_status, sizeof g_status, "ROM verificada: Inazuma Eleven 3 (BR T1.3).");
}

void launcher_init(void) { g_sel = 0; check_rom(); }

int launcher_event(int ev, const char *path, int mx, int my) {
    if (ev == EV_DROP && path) { snprintf(g_cfg.rom, sizeof g_cfg.rom, "%s", path); check_rom(); config_save(&g_cfg, g_cfg_path); return LAUNCH_CONTINUE; }
    if (ev == EV_CLICK) { int i = (my - 120) / 24; if (i >= 0 && i < ITEM_COUNT && mx > 40 && mx < 480) { g_sel = i; ev = EV_ENTER; } else return LAUNCH_CONTINUE; }
    if (ev == EV_UP) g_sel = (g_sel + ITEM_COUNT - 1) % ITEM_COUNT;
    if (ev == EV_DOWN) g_sel = (g_sel + 1) % ITEM_COUNT;
    if (ev == EV_ESC) return LAUNCH_QUIT;
    int d = ev == EV_LEFT ? -1 : (ev == EV_RIGHT || ev == EV_ENTER) ? 1 : 0;
    if (!d) return LAUNCH_CONTINUE;
    switch (g_sel) {
    case ITEM_PLAY: if (ev == EV_ENTER) { if (g_rom_ok) { config_save(&g_cfg, g_cfg_path); return LAUNCH_PLAY; } } break;
    case ITEM_QUIT: if (ev == EV_ENTER) return LAUNCH_QUIT; break;
    case ITEM_FPS: { int i = 0, n = 5; while (i < n && fps_opts[i] != g_cfg.fps) i++; i = ((i < n ? i : 1) + d + n) % n; g_cfg.fps = fps_opts[i]; break; }
    case ITEM_SCALE: g_cfg.scale += d; if (g_cfg.scale < 1) g_cfg.scale = 6; if (g_cfg.scale > 6) g_cfg.scale = 1; break;
    case ITEM_FULL: g_cfg.fullscreen ^= 1; break;
    case ITEM_LAYOUT: g_cfg.layout = (g_cfg.layout + d + 3) % 3; break;
    case ITEM_SMOOTH: g_cfg.smooth ^= 1; break;
    case ITEM_VSYNC: g_cfg.vsync ^= 1; break;
    }
    config_save(&g_cfg, g_cfg_path);
    return LAUNCH_CONTINUE;
}

/* UTF-8 -> Latin-1 (suficiente para português) */
void draw_text(uint32_t *fb, int fbw, int fbh, int x, int y, const char *s, uint32_t color) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned code = *p++;
        if (code >= 0xC0 && (*p & 0xC0) == 0x80) { code = ((code & 0x1F) << 6) | (*p++ & 0x3F); }
        if (code < 32 || code > 255) code = '?';
        const uint8_t *g = g_font[code - 32];
        for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++)
            if (g[r] & (0x80 >> b)) { int px = x + b, py = y + r; if (px >= 0 && py >= 0 && px < fbw && py < fbh) fb[py * fbw + px] = color; }
        x += 8;
    }
}
static void rect(uint32_t *fb, int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) if (i >= 0 && j >= 0 && i < LAUNCHER_W && j < LAUNCHER_H) fb[j * LAUNCHER_W + i] = c;
}

void launcher_draw(uint32_t *fb) {
    const uint32_t bg = 0xFF10233F, panel = 0xFF1A3560, hi = 0xFFF5B800, fg = 0xFFE8EEF6, dim = 0xFF8FA3BF, ok = 0xFF6FD08C, bad = 0xFFFF7A6B;
    rect(fb, 0, 0, LAUNCHER_W, LAUNCHER_H, bg);
    rect(fb, 0, 0, LAUNCHER_W, 56, panel);
    rect(fb, 0, 56, LAUNCHER_W, 2, hi);
    draw_text(fb, LAUNCHER_W, LAUNCHER_H, 24, 12, "INAZUMA ELEVEN 3", hi);
    draw_text(fb, LAUNCHER_W, LAUNCHER_H, 24, 30, "Recompilação para PC", fg);
    char line[600];
    const char *rom = g_cfg.rom[0] ? g_cfg.rom : "(nenhuma)";
    for (const char *q = rom; *q; q++) if ((*q == '/' || *q == '\\') && q[1]) rom = q + 1; /* só o nome do arquivo */
    size_t n = strlen(rom);
    snprintf(line, sizeof line, "ROM: %s%s", n > 52 ? "..." : "", n > 52 ? rom + n - 52 : rom);
    draw_text(fb, LAUNCHER_W, LAUNCHER_H, 24, 70, line, fg);
    draw_text(fb, LAUNCHER_W, LAUNCHER_H, 24, 90, g_status, g_rom_ok ? ok : bad);
    static const char *lay[] = {"vertical", "lado a lado", "só a de cima"};
    char opts[ITEM_COUNT][80];
    snprintf(opts[ITEM_PLAY], 80, g_rom_ok ? "Jogar" : "Jogar (falta a ROM)");
    if (g_cfg.fps) snprintf(opts[ITEM_FPS], 80, "Limite de quadros: %d por segundo", g_cfg.fps);
    else snprintf(opts[ITEM_FPS], 80, "Limite de quadros: sem limite");
    snprintf(opts[ITEM_SCALE], 80, "Escala da janela: %dx (%dx%d)", g_cfg.scale,
             256 * g_cfg.scale * (g_cfg.layout == 1 ? 2 : 1), 192 * g_cfg.scale * (g_cfg.layout == 0 ? 2 : 1));
    snprintf(opts[ITEM_FULL], 80, "Tela cheia: %s", g_cfg.fullscreen ? "sim" : "não");
    snprintf(opts[ITEM_LAYOUT], 80, "Telas: %s", lay[g_cfg.layout]);
    snprintf(opts[ITEM_SMOOTH], 80, "Suavizar imagem: %s", g_cfg.smooth ? "sim" : "não");
    snprintf(opts[ITEM_VSYNC], 80, "V-Sync: %s", g_cfg.vsync ? "sim" : "não");
    snprintf(opts[ITEM_QUIT], 80, "Sair");
    for (int i = 0; i < ITEM_COUNT; i++) {
        int y = 120 + i * 24;
        if (i == g_sel) { rect(fb, 40, y - 3, 440, 22, panel); draw_text(fb, LAUNCHER_W, LAUNCHER_H, 46, y, ">", hi); }
        draw_text(fb, LAUNCHER_W, LAUNCHER_H, 62, y, opts[i], i == g_sel ? hi : fg);
    }
    draw_text(fb, LAUNCHER_W, LAUNCHER_H, 24, 314, "No jogo: setas, X=A Z=B S=X A=Y Q=L W=R, mouse=toque", dim);
    draw_text(fb, LAUNCHER_W, LAUNCHER_H, 24, 332, "Enter=START Backspace=SELECT Tab=troca F11=tela cheia", dim);
    draw_text(fb, LAUNCHER_W, LAUNCHER_H, 24, 358, "Setas: escolher e mudar   Enter: confirmar   Esc: sair", dim);
}
