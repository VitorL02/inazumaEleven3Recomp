/* Launcher no estilo dos recomps: fundo em redemoinho azul com raios, título, menu lateral e
 * um painel por aba (Jogar, Gráficos, Controles, Sobre).
 *
 * A arte do jogo mostrada na aba Jogar NÃO vem do repositório: quando a ROM é verificada, o launcher
 * roda o próprio recomp sem janela até a tela título (ie3 --capturar-titulo) e guarda a imagem num
 * cache ao lado do .ini. Tudo o que é do jogo sai da ROM do usuário. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include "platform.h"
#include "launcher.h"
#include "config.h"
#include "rom.h"
#include "fonts_aa.h"

#define EXPECTED_SHA1 "a70e9a336f6ea8ff78003938dcd024533f034a68"
#define W LAUNCHER_W
#define H LAUNCHER_H
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* ------------------------------------------------------------------ estado */
enum { TAB_PLAY, TAB_GFX, TAB_CTRL, TAB_ABOUT, TAB_QUIT, TAB_COUNT };
static const char *tab_names[TAB_COUNT] = {"Jogar", "Gráficos", "Controles", "Sobre", "Sair"};
enum { ROW_FPS, ROW_SCALE, ROW_LAYOUT, ROW_FULL, ROW_SMOOTH, ROW_VSYNC, ROW_COUNT };
static const int fps_opts[] = {30, 60, 120, 144, 0};

static int g_tab, g_focus_panel, g_row;
static int g_hover_action = -1, g_hover_arg = -1;
static char g_status[256];
static int g_rom_ok;
static char g_self[1024];

/* arte capturada da ROM (tela de cima, 256x192) */
static uint32_t g_art[256 * 192];
static int g_art_ok;
static intptr_t g_art_pid;
static char g_art_path[1200];
static time_t g_art_started;

/* regiões clicáveis, montadas a cada desenho */
enum { A_TAB, A_PLAY, A_ROW, A_DEC, A_INC };
typedef struct { int x, y, w, h, action, arg; } Hit;
static Hit g_hits[64];
static int g_nhits;
static void hit(int x, int y, int w, int h, int action, int arg) {
    if (g_nhits < 64) g_hits[g_nhits++] = (Hit){x, y, w, h, action, arg};
}

/* ------------------------------------------------------------------ desenho */
static uint32_t *FB;

static inline void blend(int x, int y, uint32_t col, int a) {
    if ((unsigned)x >= W || (unsigned)y >= H || a <= 0) return;
    uint32_t *p = &FB[y * W + x];
    if (a >= 255) { *p = 0xFF000000u | col; return; }
    uint32_t d = *p;
    int r = (((col >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    int g = (((col >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    int b = ((col & 255) * a + (d & 255) * (255 - a)) / 255;
    *p = 0xFF000000u | RGB(r, g, b);
}
static uint32_t lerp_col(uint32_t a, uint32_t b, float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    int r = ((a >> 16) & 255) + (int)((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
    int g = ((a >> 8) & 255) + (int)((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
    int bl = (a & 255) + (int)((((int)b & 255) - (int)(a & 255)) * t);
    return RGB(r, g, bl);
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* distância com sinal até um retângulo de cantos arredondados */
static float rr_dist(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w / 2, cy = y + h / 2;
    float qx = fabsf(px - cx) - (w / 2 - r), qy = fabsf(py - cy) - (h / 2 - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy; if (in > 0) in = 0;
    return sqrtf(ox * ox + oy * oy) + in - r;
}
/* retângulo arredondado com gradiente vertical e borda opcional */
static void rrect(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int a, uint32_t border, int ba, float bw) {
    for (int j = y - 1; j <= y + h; j++) {
        uint32_t col = lerp_col(top, bot, (float)(j - y) / (h > 1 ? h - 1 : 1));
        for (int i = x - 1; i <= x + w; i++) {
            float d = rr_dist(i + 0.5f, j + 0.5f, x, y, w, h, r);
            float cov = clampf(0.5f - d, 0, 1);
            if (cov > 0 && a) blend(i, j, col, (int)(a * cov));
            if (ba && bw > 0) {
                float c2 = clampf(bw / 2 - fabsf(d + bw / 2) + 0.5f, 0, 1);
                if (c2 > 0) blend(i, j, border, (int)(ba * c2));
            }
        }
    }
}
/* polígono com antisserrilhado (4x4 amostras por pixel) */
static void poly(const float *xy, int n, uint32_t col, int a) {
    float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < n; i++) { if (xy[2*i] < x0) x0 = xy[2*i]; if (xy[2*i] > x1) x1 = xy[2*i]; if (xy[2*i+1] < y0) y0 = xy[2*i+1]; if (xy[2*i+1] > y1) y1 = xy[2*i+1]; }
    for (int py = (int)y0; py <= (int)y1; py++)
        for (int px = (int)x0; px <= (int)x1; px++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                float tx = px + (sx + 0.5f) / 4, ty = py + (sy + 0.5f) / 4;
                int c = 0;
                for (int i = 0, j = n - 1; i < n; j = i++) {
                    float xi = xy[2*i], yi = xy[2*i+1], xj = xy[2*j], yj = xy[2*j+1];
                    if ((yi > ty) != (yj > ty) && tx < (xj - xi) * (ty - yi) / (yj - yi) + xi) c ^= 1;
                }
                inside += c;
            }
            if (inside) blend(px, py, col, a * inside / 16);
        }
}
/* raio estilizado (ícone) com canto superior esquerdo em (x,y) e altura s */
static void bolt_icon(float x, float y, float s, uint32_t col, int a) {
    float k = s / 20.0f;
    float p[] = {x + 11*k, y, x + 2*k, y + 11*k, x + 8*k, y + 11*k, x + 5*k, y + 20*k, x + 15*k, y + 7*k, x + 9*k, y + 7*k};
    poly(p, 6, col, a);
}

/* UTF-8 -> Latin-1 */
static const char *next_ch(const char *s, unsigned *code) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned c = *p++;
    if (c >= 0xC0 && (*p & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
    else if (c >= 0xE0) { while ((*p & 0xC0) == 0x80) p++; c = '-'; } /* travessão e afins */
    if (c < 32 || c > 255) c = '?';
    *code = c;
    return (const char *)p;
}
static int text_w(int f, const char *s) {
    int w = 0; unsigned c;
    while (*s) { s = next_ch(s, &c); w += g_fonts[f].g[c - 32].adv; }
    return w;
}
/* texto com gradiente vertical (top->bot); y é o topo da linha */
static void text_g(int f, int x, int y, const char *s, uint32_t top, uint32_t bot, int a) {
    const AAFont *F = &g_fonts[f]; unsigned c;
    while (*s) {
        s = next_ch(s, &c);
        const AAGlyph *g = &F->g[c - 32];
        const uint8_t *px = F->px + g->off;
        for (int j = 0; j < g->h; j++) {
            uint32_t col = top == bot ? top : lerp_col(top, bot, (float)(g->oy + j) / F->line);
            for (int i = 0; i < g->w; i++) if (px[j * g->w + i]) blend(x + g->ox + i, y + g->oy + j, col, px[j * g->w + i] * a / 255);
        }
        x += g->adv;
    }
}
static void text(int f, int x, int y, const char *s, uint32_t col, int a) { text_g(f, x, y, s, col, col, a); }
static void text_outline(int f, int x, int y, const char *s, uint32_t col, int rad, int a) {
    for (int dy = -rad; dy <= rad; dy++) for (int dx = -rad; dx <= rad; dx++)
        if (dx * dx + dy * dy <= rad * rad + 1) text(f, x + dx, y + dy, s, col, a);
}
/* quebra de linha simples; devolve a altura usada */
static int text_wrap(int f, int x, int y, int w, const char *s, uint32_t col, int a) {
    char line[256]; int n = 0, yy = y; const char *word = s;
    line[0] = 0;
    while (*word) {
        const char *e = word; while (*e && *e != ' ') e++;
        char cand[256]; snprintf(cand, sizeof cand, "%s%s%.*s", line, n ? " " : "", (int)(e - word), word);
        if (n && text_w(f, cand) > w) { text(f, x, yy, line, col, a); yy += g_fonts[f].line + 3; snprintf(line, sizeof line, "%.*s", (int)(e - word), word); }
        else snprintf(line, sizeof line, "%s", cand);
        n = 1;
        word = *e ? e + 1 : e;
    }
    if (n) { text(f, x, yy, line, col, a); yy += g_fonts[f].line + 3; }
    return yy - y;
}
/* encurta com "..." no começo para caber em w */
static void fit_left(int f, const char *s, int w, char *out, size_t sz) {
    if (text_w(f, s) <= w) { snprintf(out, sz, "%s", s); return; }
    size_t n = strlen(s);
    for (size_t k = 0; k < n; k++) {
        if (((unsigned char)s[k] & 0xC0) == 0x80) continue;
        snprintf(out, sz, "...%s", s + k);
        if (text_w(f, out) <= w) return;
    }
}

/* ------------------------------------------------------------------ fundo (gerado uma vez) */
static uint32_t g_bg[W * H];
static int g_bg_ok;
static float frand(unsigned *s) { *s = *s * 1103515245u + 12345u; return ((*s >> 8) & 0xFFFF) / 65535.0f; }

static void add_glow(float *acc, float x0, float y0, float x1, float y1, float core, float glow, const float col[3], const float core_col[3]) {
    int bx0 = (int)fminf(x0, x1) - 40, bx1 = (int)fmaxf(x0, x1) + 40, by0 = (int)fminf(y0, y1) - 40, by1 = (int)fmaxf(y0, y1) + 40;
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy + 1e-6f;
    for (int y = by0 < 0 ? 0 : by0; y <= by1 && y < H; y++)
        for (int x = bx0 < 0 ? 0 : bx0; x <= bx1 && x < W; x++) {
            float t = clampf(((x - x0) * dx + (y - y0) * dy) / l2, 0, 1);
            float ex = x0 + t * dx - x, ey = y0 + t * dy - y, d = sqrtf(ex * ex + ey * ey);
            float g = expf(-d * d / (2 * glow * glow)) * 0.55f, c = clampf(core - d + 0.5f, 0, 1);
            float *p = &acc[(y * W + x) * 3];
            for (int k = 0; k < 3; k++) { float v = col[k] * g + core_col[k] * c; if (v > p[k]) p[k] = v; }
        }
}
static void bolt_path(float *acc, float x0, float y0, float x1, float y1, unsigned seed, float core, float glow, const float col[3]) {
    float px[65], py[65]; int n = 64;
    px[0] = x0; py[0] = y0; px[n] = x1; py[n] = y1;
    /* deslocamento do ponto médio: zigue-zague de raio */
    for (int step = n; step > 1; step /= 2) {
        float amp = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) * step / n * 0.22f;
        for (int i = 0; i < n; i += step) {
            int m = i + step / 2;
            float nx = -(py[i + step] - py[i]), ny = px[i + step] - px[i], nl = sqrtf(nx * nx + ny * ny) + 1e-6f;
            float off = (frand(&seed) - 0.5f) * 2 * amp;
            px[m] = (px[i] + px[i + step]) / 2 + nx / nl * off;
            py[m] = (py[i] + py[i + step]) / 2 + ny / nl * off;
        }
    }
    static const float white[3] = {1.0f, 1.0f, 0.95f};
    for (int i = 0; i < n; i++) add_glow(acc, px[i], py[i], px[i + 1], py[i + 1], core, glow, col, white);
}
static void build_background(void) {
    static float acc[W * H * 3];
    memset(acc, 0, sizeof acc);
    const float blue[3] = {0.35f, 0.75f, 1.0f}, cyan[3] = {0.55f, 0.95f, 1.0f};
    bolt_path(acc, 560, -20, 720, 560, 7, 1.6f, 9, blue);
    bolt_path(acc, 980, 120, 640, 300, 21, 1.3f, 8, cyan);
    bolt_path(acc, 870, 560, 960, 330, 5, 1.1f, 7, blue);
    bolt_path(acc, 330, 560, 520, 380, 13, 1.0f, 7, blue);
    const float cx = 650, cy = 255;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float dx = x - cx, dy = (y - cy) * 1.25f, r = sqrtf(dx * dx + dy * dy), th = atan2f(dy, dx);
            float phi = th + 2.1f * logf(r + 25);
            float s1 = powf(0.5f + 0.5f * sinf(phi * 5 + 1.4f * sinf(phi * 2)), 5);
            float s2 = powf(0.5f + 0.5f * sinf(phi * 21 + r * 0.015f), 14);
            float k = expf(-r / 300);
            float R = 10 + 30 * k, G = 34 + 95 * k, B = 80 + 150 * k;
            float st = (s1 * 0.55f + s2 * 0.4f) * (0.25f + 0.75f * k);
            R += 150 * st; G += 205 * st; B += 240 * st;
            float c = expf(-r / 60) * 0.7f; R += 210 * c; G += 240 * c; B += 255 * c;
            float *p = &acc[(y * W + x) * 3];
            R += p[0] * 255; G += p[1] * 255; B += p[2] * 255;
            /* lado esquerdo e base mais escuros para o texto */
            float dark = clampf(1.05f - x / 470.0f, 0, 0.82f);
            float vig = clampf((y - 380) / 200.0f, 0, 0.6f);
            float m = fmaxf(dark, vig);
            R = R * (1 - m) + 4 * m; G = G * (1 - m) + 14 * m; B = B * (1 - m) + 38 * m;
            g_bg[y * W + x] = 0xFF000000u | RGB((int)clampf(R, 0, 255), (int)clampf(G, 0, 255), (int)clampf(B, 0, 255));
        }
    g_bg_ok = 1;
}

/* ------------------------------------------------------------------ ROM e arte */
static void art_path_for(const char *sha) {
    char dir[1024]; snprintf(dir, sizeof dir, "%s", g_cfg_path);
    char *sl = strrchr(dir, '/'); if (!sl) sl = strrchr(dir, '\\');
    if (sl) sl[1] = 0; else dir[0] = 0;
    snprintf(g_art_path, sizeof g_art_path, "%sie3_titulo_%.8s.ppm", dir, sha);
}
static int load_art(void) {
    FILE *f = fopen(g_art_path, "rb"); if (!f) return 0;
    int w, h, mx;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w != 256 || h < 192) { fclose(f); return 0; }
    fgetc(f);
    for (int i = 0; i < 256 * 192; i++) { int r = fgetc(f), g = fgetc(f), b = fgetc(f); if (b < 0) { fclose(f); return 0; } g_art[i] = RGB(r, g, b); }
    fclose(f);
    return g_art_ok = 1;
}
static int g_art_allowed = 1;
static void start_art_capture(void) {
    if (g_art_pid || !g_self[0] || !g_art_allowed) return;
    /* outra captura em andamento (outro launcher aberto)? */
    char tmp[1300]; struct stat st; snprintf(tmp, sizeof tmp, "%s.tmp", g_art_path);
    if (stat(tmp, &st) == 0 && time(NULL) - st.st_mtime < 90) return;
    const char *argv[] = {"ie3", "--capturar-titulo", g_art_path, "--config", g_cfg_path, NULL};
    g_art_pid = plat_spawn_quiet(g_self, argv);
    if (g_art_pid) g_art_started = time(NULL);
}
static void check_rom(void) {
    g_art_ok = 0;
    if (!g_cfg.rom[0]) { g_rom_ok = 0; snprintf(g_status, sizeof g_status, "Nenhuma ROM escolhida"); return; }
    char err[256];
    if (rom_open(g_cfg.rom, err, sizeof err)) { g_rom_ok = 0; snprintf(g_status, sizeof g_status, "ROM inválida: %s", err); return; }
    if (strcmp(g_romfile.sha1, EXPECTED_SHA1)) {
        g_rom_ok = 0;
        snprintf(g_status, sizeof g_status, "Versão diferente da esperada (precisa da tradução BR T1.3)");
        return;
    }
    g_rom_ok = 1; snprintf(g_status, sizeof g_status, "ROM verificada (tradução BR T1.3)");
    art_path_for(g_romfile.sha1);
    if (!load_art()) start_art_capture();
}
void launcher_tick(void) {
    if (!g_rom_ok || g_art_ok || !g_art_pid) return;
    struct stat st;
    if (stat(g_art_path, &st) == 0 && load_art()) { plat_proc_done(g_art_pid); g_art_pid = 0; return; }
    if (plat_proc_done(g_art_pid)) { g_art_pid = 0; if (!load_art()) g_art_started = 0; return; }
    if (time(NULL) - g_art_started > 60) { plat_proc_kill(g_art_pid); g_art_pid = 0; g_art_started = 0; }
}
void launcher_wait_art(int secs) {
    for (int i = 0; i < secs * 10 && g_art_pid && !g_art_ok; i++) { launcher_tick(); plat_sleep(0.1); }
}

void launcher_init(const char *self_exe) {
    snprintf(g_self, sizeof g_self, "%s", self_exe ? self_exe : "");
    /* no modo de teste (RECOMP_LAUNCHER_DUMP) só captura se pedirem para esperar */
    if (getenv("RECOMP_LAUNCHER_DUMP") && !getenv("RECOMP_LAUNCHER_WAIT_ART")) g_art_allowed = 0;
    g_tab = TAB_PLAY; g_focus_panel = 0; g_row = 0;
    check_rom();
}

/* ------------------------------------------------------------------ eventos */
static void change_row(int row, int d) {
    switch (row) {
    case ROW_FPS: { int i = 0, n = 5; while (i < n && fps_opts[i] != g_cfg.fps) i++; i = ((i < n ? i : 1) + d + n) % n; g_cfg.fps = fps_opts[i]; break; }
    case ROW_SCALE: g_cfg.scale += d; if (g_cfg.scale < 1) g_cfg.scale = 6; if (g_cfg.scale > 6) g_cfg.scale = 1; break;
    case ROW_LAYOUT: g_cfg.layout = (g_cfg.layout + d + 3) % 3; break;
    case ROW_FULL: g_cfg.fullscreen ^= 1; break;
    case ROW_SMOOTH: g_cfg.smooth ^= 1; break;
    case ROW_VSYNC: g_cfg.vsync ^= 1; break;
    }
    config_save(&g_cfg, g_cfg_path);
}
static int try_play(void) {
    if (!g_rom_ok) { g_tab = TAB_PLAY; return LAUNCH_CONTINUE; }
    config_save(&g_cfg, g_cfg_path);
    if (g_art_pid) { plat_proc_kill(g_art_pid); g_art_pid = 0; }
    return LAUNCH_PLAY;
}
static int activate_tab(int t) {
    g_tab = t;
    if (t == TAB_QUIT) return LAUNCH_QUIT;
    return LAUNCH_CONTINUE;
}
int launcher_event(int ev, const char *path, int mx, int my) {
    if (ev == EV_DROP && path) {
        snprintf(g_cfg.rom, sizeof g_cfg.rom, "%s", path);
        g_art_pid = 0;
        check_rom(); config_save(&g_cfg, g_cfg_path);
        g_tab = TAB_PLAY; g_focus_panel = 0;
        return LAUNCH_CONTINUE;
    }
    if (ev == EV_MOTION || ev == EV_CLICK) {
        g_hover_action = g_hover_arg = -1;
        for (int i = g_nhits - 1; i >= 0; i--) {
            Hit *h = &g_hits[i];
            if (mx >= h->x && my >= h->y && mx < h->x + h->w && my < h->y + h->h) { g_hover_action = h->action; g_hover_arg = h->arg; break; }
        }
        if (ev == EV_MOTION || g_hover_action < 0) return LAUNCH_CONTINUE;
        switch (g_hover_action) {
        case A_TAB: g_focus_panel = 0; return activate_tab(g_hover_arg);
        case A_PLAY: return try_play();
        case A_ROW: g_focus_panel = 1; g_row = g_hover_arg; change_row(g_row, 1); break;
        case A_DEC: g_focus_panel = 1; g_row = g_hover_arg; change_row(g_row, -1); break;
        case A_INC: g_focus_panel = 1; g_row = g_hover_arg; change_row(g_row, 1); break;
        }
        return LAUNCH_CONTINUE;
    }
    if (!g_focus_panel) {
        switch (ev) {
        case EV_UP: g_tab = (g_tab + TAB_COUNT - 1) % TAB_COUNT; break;
        case EV_DOWN: g_tab = (g_tab + 1) % TAB_COUNT; break;
        case EV_ESC: return LAUNCH_QUIT;
        case EV_RIGHT: case EV_ENTER:
            if (g_tab == TAB_PLAY) return ev == EV_ENTER ? try_play() : LAUNCH_CONTINUE;
            if (g_tab == TAB_QUIT) return ev == EV_ENTER ? LAUNCH_QUIT : LAUNCH_CONTINUE;
            if (g_tab == TAB_GFX) { g_focus_panel = 1; g_row = 0; }
            break;
        }
        return LAUNCH_CONTINUE;
    }
    switch (ev) {
    case EV_ESC: g_focus_panel = 0; break;
    case EV_UP: if (g_row == 0) g_focus_panel = 0; else g_row--; break;
    case EV_DOWN: if (g_row < ROW_COUNT - 1) g_row++; break;
    case EV_LEFT: change_row(g_row, -1); break;
    case EV_RIGHT: case EV_ENTER: change_row(g_row, 1); break;
    }
    return LAUNCH_CONTINUE;
}

/* ------------------------------------------------------------------ telas */
static const uint32_t C_TEXT = RGB(232, 242, 255), C_DIM = RGB(150, 180, 215), C_NAVY = RGB(10, 26, 60);
static const uint32_t C_Y1 = RGB(255, 232, 90), C_Y2 = RGB(255, 150, 22), C_OK = RGB(110, 225, 140), C_BAD = RGB(255, 120, 105);

static void chevron(float x, float y, int dir, uint32_t col, int a) {
    float p[] = {x, y - 7, x + 8 * dir, y, x, y + 7};
    if (dir < 0) { p[0] = x; p[2] = x - 8; p[4] = x; }
    poly(p, 3, col, a);
}
static void toggle(int x, int y, int on) {
    rrect(x, y, 48, 24, 12, on ? C_Y1 : RGB(70, 90, 120), on ? C_Y2 : RGB(50, 66, 95), 255, 0, 0, 0);
    int kx = on ? x + 26 : x + 2;
    rrect(kx, y + 2, 20, 20, 10, RGB(255, 255, 255), RGB(220, 230, 245), 255, 0, 0, 0);
}
static int keycap(int x, int y, const char *s) {
    int w = text_w(FONT_SMALL, s) + 16; if (w < 30) w = 30;
    rrect(x, y, w, 24, 6, RGB(60, 90, 140), RGB(36, 58, 100), 255, RGB(120, 170, 230), 160, 1.2f);
    text(FONT_SMALL, x + (w - text_w(FONT_SMALL, s)) / 2, y + 3, s, C_TEXT, 255);
    return w;
}

static void draw_play(int px, int py, int pw, int ph) {
    int lx = px + 26, cw = 230;
    text(FONT_SMALL, lx, py + 62, "ROM", C_DIM, 255);
    char name[1100];
    const char *rom = g_cfg.rom[0] ? g_cfg.rom : "(nenhuma)";
    const char *base = rom;
    for (const char *q = rom; *q; q++) if ((*q == '/' || *q == '\\') && q[1]) base = q + 1;
    fit_left(FONT_BODY, base, cw, name, sizeof name);
    text(FONT_BODY, lx, py + 82, name, C_TEXT, 255);
    rrect(lx, py + 116, 10, 10, 5, g_rom_ok ? C_OK : C_BAD, g_rom_ok ? C_OK : C_BAD, 255, 0, 0, 0);
    int sh = text_wrap(FONT_SMALL, lx + 18, py + 110, cw - 18, g_status, g_rom_ok ? C_OK : C_BAD, 255);
    if (!g_rom_ok) {
        int by = py + 116 + sh;
        rrect(lx, by, cw, 74, 10, RGB(20, 50, 100), RGB(14, 36, 76), 150, RGB(120, 190, 255), 200, 1.5f);
        text_wrap(FONT_SMALL, lx + 14, by + 12, cw - 28, "Arraste o arquivo .nds do jogo para esta janela", C_TEXT, 255);
    }
    /* botão Jogar */
    int bx = lx, by = py + ph - 82, bw = cw, bh = 56;
    int hov = g_hover_action == A_PLAY, sel = !g_focus_panel && g_tab == TAB_PLAY;
    if (g_rom_ok) {
        if (sel || hov) rrect(bx - 4, by - 4, bw + 8, bh + 8, 30, C_Y1, C_Y2, 70, 0, 0, 0);
        rrect(bx, by, bw, bh, 28, hov ? RGB(255, 244, 150) : C_Y1, C_Y2, 255, RGB(255, 250, 210), 200, 1.5f);
        bolt_icon(bx + 52, by + 15, 26, C_NAVY, 255);
        text(FONT_H, bx + 84, by + 13, "JOGAR", C_NAVY, 255);
    } else {
        rrect(bx, by, bw, bh, 28, RGB(70, 85, 110), RGB(52, 64, 88), 255, 0, 0, 0);
        text(FONT_H, bx + (bw - text_w(FONT_H, "JOGAR")) / 2, by + 13, "JOGAR", RGB(150, 160, 180), 255);
    }
    hit(bx, by, bw, bh, A_PLAY, 0);
    /* arte da ROM: tela título capturada, ampliada 1,25x com filtro bilinear */
    int ax = px + pw - 26 - 288, ay = py + 54, aw = 288, ah = 216;
    rrect(ax - 6, ay - 6, aw + 12, ah + 12, 12, RGB(120, 200, 255), RGB(60, 120, 220), 90, RGB(170, 220, 255), 200, 2);
    if (g_art_ok) {
        for (int y = 0; y < ah; y++) for (int x = 0; x < aw; x++) {
            float sx = (x + 0.5f) * 256 / aw - 0.5f, sy = (y + 0.5f) * 192 / ah - 0.5f;
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy); float fx = sx - x0, fy = sy - y0;
            int x1 = x0 + 1, y1 = y0 + 1;
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 255) x1 = 255; if (y1 > 191) y1 = 191;
            uint32_t a = lerp_col(g_art[y0 * 256 + x0], g_art[y0 * 256 + x1], fx);
            uint32_t b = lerp_col(g_art[y1 * 256 + x0], g_art[y1 * 256 + x1], fx);
            blend(ax + x, ay + y, lerp_col(a, b, fy), 255);
        }
    } else {
        rrect(ax, ay, aw, ah, 6, RGB(16, 40, 84), RGB(8, 22, 52), 255, 0, 0, 0);
        bolt_icon(ax + aw / 2 - 20, ay + 70, 40, RGB(90, 150, 230), 200);
        const char *m = !g_rom_ok ? "A tela título aparece aqui" : g_art_pid ? "Preparando a prévia..." : "Prévia indisponível";
        text(FONT_SMALL, ax + (aw - text_w(FONT_SMALL, m)) / 2, ay + 140, m, C_DIM, 255);
        if (!g_rom_ok) text(FONT_SMALL, ax + (aw - text_w(FONT_SMALL, "quando a ROM for verificada")) / 2, ay + 160, "quando a ROM for verificada", C_DIM, 255);
    }
    if (g_art_ok) { const char *cap = "Imagem gerada da sua ROM"; text(FONT_SMALL, ax + aw - text_w(FONT_SMALL, cap), ay + ah + 10, cap, C_DIM, 200); }
}

static void draw_gfx(int px, int py, int pw) {
    static const char *labels[ROW_COUNT] = {"Limite de quadros", "Escala da janela", "Telas", "Tela cheia", "Suavizar imagem", "V-Sync"};
    static const char *lay[] = {"Vertical", "Lado a lado", "Só uma (Tab troca)"};
    for (int i = 0; i < ROW_COUNT; i++) {
        int ry = py + 60 + i * 38, rx = px + 18, rw = pw - 36;
        int sel = g_focus_panel && g_row == i, hov = (g_hover_action == A_ROW || g_hover_action == A_DEC || g_hover_action == A_INC) && g_hover_arg == i;
        if (sel) rrect(rx, ry, rw, 34, 10, RGB(255, 220, 80), RGB(255, 170, 30), 46, C_Y1, 170, 1.5f);
        else if (hov) rrect(rx, ry, rw, 34, 10, RGB(255, 255, 255), RGB(255, 255, 255), 22, 0, 0, 0);
        text(FONT_BODY, rx + 14, ry + 6, labels[i], sel ? C_Y1 : C_TEXT, 255);
        char v[64];
        int vx = rx + rw - 14;
        if (i >= ROW_FULL) {
            int on = i == ROW_FULL ? g_cfg.fullscreen : i == ROW_SMOOTH ? g_cfg.smooth : g_cfg.vsync;
            toggle(vx - 48, ry + 5, on);
            hit(rx, ry, rw, 34, A_ROW, i);
            continue;
        }
        if (i == ROW_FPS) { if (g_cfg.fps) snprintf(v, sizeof v, "%d por segundo", g_cfg.fps); else snprintf(v, sizeof v, "Sem limite"); }
        else if (i == ROW_SCALE) snprintf(v, sizeof v, "%dx  (%dx%d)", g_cfg.scale, 256 * g_cfg.scale * (g_cfg.layout == 1 ? 2 : 1), 192 * g_cfg.scale * (g_cfg.layout == 0 ? 2 : 1));
        else snprintf(v, sizeof v, "%s", lay[g_cfg.layout]);
        int tw = text_w(FONT_BODY, v), boxw = 220;
        int bx = vx - boxw;
        chevron(bx + 8, ry + 17, -1, sel ? C_Y1 : C_DIM, 255);
        chevron(vx - 8, ry + 17, 1, sel ? C_Y1 : C_DIM, 255);
        text(FONT_BODY, bx + (boxw - tw) / 2, ry + 6, v, C_TEXT, 255);
        hit(rx, ry, rw - boxw, 34, A_ROW, i);
        hit(bx - 6, ry, boxw / 2 + 6, 34, A_DEC, i);
        hit(bx + boxw / 2, ry, boxw / 2 + 6, 34, A_INC, i);
    }
}

static void draw_ctrl(int px, int py) {
    static const char *rows[][2] = {
        {"Direcional", "Setas"}, {"A", "X"}, {"B", "Z"}, {"X", "S"}, {"Y", "A"},
        {"L", "Q"}, {"R", "W"}, {"START", "Enter"}, {"SELECT", "Backspace"}, {"Toque", "Mouse (tela de baixo)"},
    };
    for (int i = 0; i < 10; i++) {
        int col = i / 5, ry = py + 62 + (i % 5) * 40, cx = px + 26 + col * 290;
        text(FONT_BODY, cx, ry + 2, rows[i][0], C_TEXT, 255);
        keycap(cx + 110, ry, rows[i][1]);
    }
    int y = py + 266, x = px + 26;
    static const char *extra[][2] = {{"Tab", "troca a tela"}, {"F11", "tela cheia"}, {"Esc", "sair do jogo"}};
    for (int i = 0; i < 3; i++) {
        x += keycap(x, y, extra[i][0]) + 8;
        text(FONT_SMALL, x, y + 3, extra[i][1], C_DIM, 255);
        x += text_w(FONT_SMALL, extra[i][1]) + 28;
    }
}

static void draw_about(int px, int py, int pw) {
    int x = px + 26, w = pw - 52, y = py + 60;
    y += text_wrap(FONT_BODY, x, y, w, "Recompilação estática de Inazuma Eleven 3: Ogros ao Ataque! (Nintendo DS). O código do jogo foi traduzido de ARM para C e compilado como um programa nativo do PC. Não é emulação: a sua ROM fornece os dados do jogo (gráficos, textos e sons) e o executável confere se ela é a versão certa.", C_TEXT, 255) + 10;
    y += text_wrap(FONT_SMALL, x, y, w, "Este projeto não inclui nenhum arquivo do jogo: você precisa da sua própria cópia. A tradução BR é do Grupo The Ogre Traduction / CyberLife.", C_DIM, 255) + 10;
    text_wrap(FONT_SMALL, x, y, w, "Em desenvolvimento: 3D, som e vídeos ainda não funcionam.", C_Y1, 255);
}

void launcher_draw(uint32_t *fb) {
    FB = fb; g_nhits = 0;
    if (!g_bg_ok) build_background();
    memcpy(fb, g_bg, sizeof g_bg);

    /* título */
    const char *t1 = "INAZUMA ELEVEN 3";
    text_outline(FONT_TITLE, 40, 22, t1, RGB(6, 16, 44), 4, 255);
    text_g(FONT_TITLE, 40, 22, t1, C_Y1, C_Y2, 255);
    const char *t2 = "OGROS AO ATAQUE!";
    int pw2 = text_w(FONT_H, t2) + 56;
    rrect(40, 84, pw2, 36, 18, RGB(214, 46, 150), RGB(118, 32, 150), 255, RGB(255, 170, 230), 220, 2);
    bolt_icon(52, 92, 20, RGB(255, 230, 90), 255);
    text_outline(FONT_H, 80, 88, t2, RGB(60, 10, 70), 2, 255);
    text(FONT_H, 80, 88, t2, RGB(255, 255, 255), 255);
    text(FONT_SMALL, 42, 132, "Recompilação estática para PC", C_DIM, 255);

    /* menu lateral */
    for (int i = 0; i < TAB_COUNT; i++) {
        int x = 40, y = 182 + i * 54, w = 236, h = 44;
        int sel = g_tab == i, hov = g_hover_action == A_TAB && g_hover_arg == i;
        if (sel) {
            if (!g_focus_panel) rrect(x - 4, y - 4, w + 8, h + 8, 26, C_Y1, C_Y2, 60, 0, 0, 0);
            rrect(x, y, w, h, 22, g_focus_panel ? RGB(230, 205, 90) : C_Y1, g_focus_panel ? RGB(220, 140, 30) : C_Y2, 255, RGB(255, 250, 210), 200, 1.5f);
            bolt_icon(x + 18, y + 12, 20, C_NAVY, 255);
            text(FONT_MENU, x + 48, y + 11, tab_names[i], C_NAVY, 255);
        } else {
            rrect(x, y, w, h, 22, RGB(16, 40, 86), RGB(8, 24, 58), hov ? 230 : 160, RGB(110, 170, 240), hov ? 200 : 90, 1.2f);
            text(FONT_MENU, x + 48, y + 11, tab_names[i], hov ? RGB(255, 255, 255) : C_TEXT, 255);
        }
        hit(x, y, w, h, A_TAB, i);
    }

    /* painel da aba */
    int px = 310, py = 172, pw = 610, ph = 318;
    rrect(px, py, pw, ph, 18, RGB(10, 30, 70), RGB(4, 14, 38), 205, RGB(110, 190, 255), 130, 2);
    int tab = g_tab == TAB_QUIT ? TAB_PLAY : g_tab;
    if (g_tab == TAB_QUIT) {
        text(FONT_H, px + 26, py + 20, "Sair", C_TEXT, 255);
        text(FONT_BODY, px + 26, py + 64, "Pressione Enter (ou clique em Sair) para fechar.", C_DIM, 255);
    } else {
        text(FONT_H, px + 26, py + 20, tab_names[tab], C_TEXT, 255);
        rrect(px + 26, py + 52, 40, 3, 1, C_Y1, C_Y2, 255, 0, 0, 0);
        if (tab == TAB_PLAY) draw_play(px, py, pw, ph);
        else if (tab == TAB_GFX) draw_gfx(px, py, pw);
        else if (tab == TAB_CTRL) draw_ctrl(px, py);
        else draw_about(px, py, pw);
    }

    /* rodapé */
    for (int y = 492; y < H; y++) for (int x = 0; x < W; x++) blend(x, y, RGB(3, 10, 28), 120 + (y - 492) * 2);
    for (int x = 0; x < W; x++) blend(x, 492, RGB(110, 190, 255), 50);
    const char *hint = g_focus_panel ? "Cima/baixo: opção    Esquerda/direita: mudar    Esc: voltar ao menu"
                                     : "Cima/baixo: menu    Enter: escolher    Esc: sair    Mouse também funciona";
    text(FONT_SMALL, 40, 508, hint, C_DIM, 230);
    const char *ver = "v0.2 · em desenvolvimento";
    text(FONT_SMALL, W - 40 - text_w(FONT_SMALL, ver), 508, ver, C_DIM, 180);
}
