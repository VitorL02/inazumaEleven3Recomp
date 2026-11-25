/* Ponto de entrada do PC: monta a memória como o bootloader do DS deixaria e roda o crt0 recompilado. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "platform.h"
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#endif

static Cpu *g_cpu;
void rt_backtrace(Cpu *c);
/* RECOMP_PROFILE: amostra o endereço de retorno no topo da pilha a cada 1 ms (só Linux/macOS) */
#define PROF_N 4096
static uint32_t g_prof_addr[PROF_N], g_prof_cnt[PROF_N];
static void on_prof(int sig) {
    (void)sig;
    if (!g_cpu || g_cpu->rsp <= 0) return;
    for (int d = 1; d <= 3 && d <= g_cpu->rsp; d++) {
        uint32_t a = g_cpu->rs[g_cpu->rsp - d] ^ (d << 28);
        uint32_t h = (a * 2654435761u) % PROF_N;
        for (int i = 0; i < PROF_N; i++, h = (h + 1) % PROF_N) {
            if (g_prof_addr[h] == a || !g_prof_cnt[h]) { g_prof_addr[h] = a; g_prof_cnt[h]++; break; }
        }
    }
}
static void prof_dump(void) {
    for (int d = 1; d <= 3; d++) {
        fprintf(stderr, "  perfil (profundidade %d):", d);
        for (int k = 0; k < 8; k++) {
            int best = -1;
            for (int i = 0; i < PROF_N; i++) if (g_prof_cnt[i] && (g_prof_addr[i] >> 28) == (uint32_t)d && (best < 0 || g_prof_cnt[i] > g_prof_cnt[best])) best = i;
            if (best < 0) break;
            fprintf(stderr, " %07x:%u", g_prof_addr[best] & 0x0FFFFFFF, g_prof_cnt[best]); g_prof_cnt[best] = 0;
        }
        fprintf(stderr, "\n");
    }
}
#ifndef _WIN32
static void on_alarm(int sig) {
    (void)sig;
    fprintf(stderr, "\n[recomp] watchdog: travado. Último lr=%08x sp=%08x\n", g_cpu->r[14], g_cpu->r[13]);
    rt_backtrace(g_cpu);
    extern void rt_dump_threads(void);
    fprintf(stderr, "  threads:\n"); rt_dump_threads();
    if (getenv("RECOMP_PROFILE")) prof_dump();
    _exit(2);
}
#endif

void rt_init_dispatch(void);
void f_arm9_02000800(Cpu *c);  /* _start (crt0) */
extern int g_trace;

static void put32(uint32_t a, uint32_t v) { memcpy(g_main + (a & 0x3FFFFF), &v, 4); }

#include <time.h>
#include "video.h"
#include "rom.h"
#include "snd.h"
#include "config.h"
#include "launcher.h"
extern uint64_t g_frame;
extern uint32_t g_keys; extern int g_touch, g_touch_x, g_touch_y;
const uint8_t *rt_io(void);
static uint32_t g_fb[256 * 384];
static int g_swap; /* com "só uma tela": Tab mostra a de baixo */

static double now_sec(void) { return plat_now(); }
/* Limite de quadros: espera até a hora do próximo quadro. */
static void frame_pace(void) {
    static double next;
    if (g_cfg.fps <= 0) return;
    double per = 1.0 / g_cfg.fps, t = now_sec();
    if (next == 0 || t - next > 0.25) next = t; /* atrasou demais: não tenta compensar */
    next += per;
    double wait = next - now_sec();
    plat_sleep(wait);
}

/* Tamanho lógico da imagem do jogo conforme o layout */
static void layout_size(int *w, int *h) {
    *w = g_cfg.layout == 1 ? 512 : 256;
    *h = g_cfg.layout == 0 ? 384 : 192;
}

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
static SDL_Window *g_win; static SDL_Renderer *g_ren; static SDL_Texture *g_tex;
static int g_turbo; /* Espaço segurado: roda sem esperar o relógio (pulo de quadros) */
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream; int frames = len / 4;
    int n = snd_read(out, frames);
    if (n < frames) memset(out + n * 2, 0, (size_t)(frames - n) * 4);
}
static void sdl_audio(void) {
    static int opened;
    if (opened || getenv("RECOMP_MUDO")) return;
    opened = 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
    SDL_AudioSpec want = {0}, have;
    want.freq = SND_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    else fprintf(stderr, "[recomp] sem saída de áudio: %s\n", SDL_GetError());
}
static void sdl_window(int w, int h, int scale) {
    SDL_Init(SDL_INIT_VIDEO);
    sdl_audio();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_cfg.smooth ? "1" : "0");
    Uint32 fl = SDL_WINDOW_RESIZABLE | (g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_Rect ub; /* escala maior que a tela: reduz até caber */
    if (SDL_GetDisplayUsableBounds(0, &ub) == 0) while (scale > 1 && (w * scale > ub.w || h * scale > ub.h)) scale--;
    if (!g_win) g_win = SDL_CreateWindow("Inazuma Eleven 3 Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w * scale, h * scale, fl);
    else { SDL_SetWindowSize(g_win, w * scale, h * scale); SDL_SetWindowFullscreen(g_win, g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | (g_cfg.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    SDL_RenderSetLogicalSize(g_ren, w, h); /* mantém a proporção em tela cheia */
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
}
/* Teclado padrão: setas, X=A, Z=B, S=X, A=Y, Q=L, W=R, Enter=START, Backspace=SELECT. Mouse = toque. */
static void sdl_key(SDL_Keycode k, int down) {
    int bit = -1;
    switch (k) {
    case SDLK_x: bit = 0; break; case SDLK_z: bit = 1; break;
    case SDLK_BACKSPACE: case SDLK_RSHIFT: bit = 2; break; case SDLK_RETURN: bit = 3; break;
    case SDLK_RIGHT: bit = 4; break; case SDLK_LEFT: bit = 5; break; case SDLK_UP: bit = 6; break; case SDLK_DOWN: bit = 7; break;
    case SDLK_w: bit = 8; break; case SDLK_q: bit = 9; break; case SDLK_s: bit = 10; break; case SDLK_a: bit = 11; break;
    }
    if (bit >= 0) { if (down) g_keys |= 1u << bit; else g_keys &= ~(1u << bit); }
}
/* Converte a posição do mouse (coordenadas lógicas) para a tela de baixo */
static int mouse_to_touch(int x, int y, int *tx, int *ty) {
    int bx = 0, by = 192;
    if (g_cfg.layout == 1) { bx = 256; by = 0; }
    if (g_cfg.layout == 2) { if (!g_swap) return 0; bx = 0; by = 0; }
    x -= bx; y -= by;
    if (x < 0 || y < 0 || x >= 256 || y >= 192) return 0;
    *tx = x; *ty = y; return 1;
}
static void sdl_events(void) {
    SDL_Event e;
    static int mouse_down;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: exit(0);
        case SDL_KEYDOWN: case SDL_KEYUP:
            if (e.key.repeat) break;
            /* Esc não fecha mais o jogo (fechava sem querer); para sair, feche a janela (Alt+F4) */
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) { g_cfg.fullscreen ^= 1; SDL_SetWindowFullscreen(g_win, g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); break; }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_TAB) { g_swap ^= 1; break; }
            if (e.key.keysym.sym == SDLK_SPACE) { g_turbo = e.type == SDL_KEYDOWN; snd_set_turbo(g_turbo); break; }
            sdl_key(e.key.keysym.sym, e.type == SDL_KEYDOWN); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) mouse_down = e.type == SDL_MOUSEBUTTONDOWN;
            /* fallthrough */
        case SDL_MOUSEMOTION: {
            int x = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, y = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y, tx, ty;
            if (mouse_down && mouse_to_touch(x, y, &tx, &ty)) { g_touch = 1; g_touch_x = tx; g_touch_y = ty; }
            else g_touch = 0;
            break; }
        }
    }
}
#endif

/* Mostra o quadro do jogo: janela SDL2 e/ou PPM (RECOMP_DUMP=<pasta>, a cada RECOMP_DUMP_EVERY quadros). */
static int g_headless;
static const char *g_capture_path; /* --capturar-titulo: grava a tela de cima no quadro CAPTURE_FRAME e sai */
#define CAPTURE_FRAME 5400
#define TURBO_SKIP 4
static char g_self_exe[1024];
void video_present(void) {
    if (g_capture_path) {
        if (g_frame + 120 < CAPTURE_FRAME) return;
        video_render(rt_io(), g_fb, g_fb + 256 * 192); /* o renderizador guarda estado entre quadros */
        if (g_frame < CAPTURE_FRAME) return;
        char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", g_capture_path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            fprintf(f, "P6\n256 192\n255\n");
            for (int i = 0; i < 256 * 192; i++) { uint8_t px[3] = {g_fb[i] >> 16, g_fb[i] >> 8, g_fb[i]}; fwrite(px, 1, 3, f); }
            fclose(f); remove(g_capture_path); rename(tmp, g_capture_path);
        }
        _exit(0);
    }
    const char *dump = getenv("RECOMP_DUMP");
    int every = getenv("RECOMP_DUMP_EVERY") ? atoi(getenv("RECOMP_DUMP_EVERY")) : 60;
    int want_dump = dump && every > 0 && g_frame % every == 0;
    int want_win = !g_headless;
#ifdef HAVE_SDL2
    if (want_win && g_tex) {
        sdl_events();
        /* acelerado: sem espera e só 1 de cada TURBO_SKIP quadros vai para a tela */
        if (g_turbo) { if (g_frame % TURBO_SKIP && !want_dump) return; }
        else frame_pace();
    } else
#endif
    if (want_win) frame_pace();
    if (!want_dump && !want_win) return;
    video_render(rt_io(), g_fb, g_fb + 256 * 192);
    if (want_dump && getenv("RECOMP_VIDLOG")) {
        const uint8_t *io = rt_io();
        uint32_t dA, dB; uint16_t mbA, mbB; memcpy(&dA, io, 4); memcpy(&dB, io + 0x1000, 4); memcpy(&mbA, io + 0x6C, 2); memcpy(&mbB, io + 0x106C, 2);
        fprintf(stderr, "[video] quadro %llu DISPCNT A=%08x B=%08x brilho A=%04x B=%04x\n", (unsigned long long)g_frame, dA, dB, mbA, mbB);
    }
    if (want_dump) {
        char path[600]; snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dump, (unsigned long long)g_frame);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n256 384\n255\n");
            for (int i = 0; i < 256 * 384; i++) { uint8_t px[3] = {g_fb[i] >> 16, g_fb[i] >> 8, g_fb[i]}; fwrite(px, 1, 3, f); }
            fclose(f);
        }
    }
#ifdef HAVE_SDL2
    if (want_win) {
        int w, h; layout_size(&w, &h);
        if (!g_tex) { sdl_window(w, h, g_cfg.scale); sdl_events(); }
        if (g_cfg.layout == 1) { /* lado a lado: a textura tem 512x192 */
            static uint32_t side[512 * 192];
            for (int y = 0; y < 192; y++) { memcpy(side + y * 512, g_fb + y * 256, 1024); memcpy(side + y * 512 + 256, g_fb + (192 + y) * 256, 1024); }
            SDL_UpdateTexture(g_tex, NULL, side, 512 * 4);
        } else if (g_cfg.layout == 2) SDL_UpdateTexture(g_tex, NULL, g_fb + (g_swap ? 256 * 192 : 0), 256 * 4);
        else SDL_UpdateTexture(g_tex, NULL, g_fb, 256 * 4);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren); SDL_RenderCopy(g_ren, g_tex, NULL, NULL); SDL_RenderPresent(g_ren);
    }
#endif
}

static void write_ppm(const char *path, const uint32_t *fb, int w, int h) {
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t px[3] = {fb[i] >> 16, fb[i] >> 8, fb[i]}; fwrite(px, 1, 3, f); }
    fclose(f);
}

/* Launcher. Com SDL2 é interativo. Sem janela, RECOMP_LAUNCHER_DUMP=<arq.ppm> desenha a tela
 * (depois de aplicar RECOMP_LAUNCHER_KEYS="down,right,enter...") e sai: serve para teste. */
static int run_launcher(void) {
    static uint32_t fb[LAUNCHER_W * LAUNCHER_H];
    launcher_init(g_self_exe);
    const char *dump = getenv("RECOMP_LAUNCHER_DUMP");
    if (dump) {
        const char *keys = getenv("RECOMP_LAUNCHER_KEYS");
        int r = LAUNCH_CONTINUE;
        if (keys) {
            char buf[512]; snprintf(buf, sizeof buf, "%s", keys);
            for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
                int ev = !strcmp(t, "up") ? EV_UP : !strcmp(t, "down") ? EV_DOWN : !strcmp(t, "left") ? EV_LEFT :
                         !strcmp(t, "right") ? EV_RIGHT : !strcmp(t, "enter") ? EV_ENTER : !strcmp(t, "esc") ? EV_ESC : EV_NONE;
                int mx, my;
                if (!strncmp(t, "drop=", 5)) { r = launcher_event(EV_DROP, t + 5, 0, 0); continue; }
                if (sscanf(t, "click=%d:%d", &mx, &my) == 2) { launcher_draw(fb); launcher_event(EV_MOTION, NULL, mx, my); r = launcher_event(EV_CLICK, NULL, mx, my); continue; }
                if (sscanf(t, "move=%d:%d", &mx, &my) == 2) { launcher_draw(fb); r = launcher_event(EV_MOTION, NULL, mx, my); continue; }
                r = launcher_event(ev, NULL, 0, 0);
            }
        }
        if (getenv("RECOMP_LAUNCHER_WAIT_ART")) launcher_wait_art(atoi(getenv("RECOMP_LAUNCHER_WAIT_ART")));
        launcher_draw(fb); write_ppm(dump, fb, LAUNCHER_W, LAUNCHER_H);
        return r == LAUNCH_PLAY ? LAUNCH_PLAY : LAUNCH_QUIT;
    }
#ifdef HAVE_SDL2
    SDL_Init(SDL_INIT_VIDEO);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window *w = SDL_CreateWindow("Inazuma Eleven 3 Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, LAUNCHER_W, LAUNCHER_H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!r) r = SDL_CreateRenderer(w, -1, 0);
    SDL_RenderSetLogicalSize(r, LAUNCHER_W, LAUNCHER_H);
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, LAUNCHER_W, LAUNCHER_H);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    int res = LAUNCH_CONTINUE;
    while (res == LAUNCH_CONTINUE) {
        SDL_Event e;
        while (res == LAUNCH_CONTINUE && SDL_PollEvent(&e)) {
            int ev = EV_NONE;
            if (e.type == SDL_QUIT) res = LAUNCH_QUIT;
            else if (e.type == SDL_DROPFILE) { res = launcher_event(EV_DROP, e.drop.file, 0, 0); SDL_free(e.drop.file); }
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) res = launcher_event(EV_CLICK, NULL, e.button.x, e.button.y);
            else if (e.type == SDL_MOUSEMOTION) launcher_event(EV_MOTION, NULL, e.motion.x, e.motion.y);
            else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_UP: ev = EV_UP; break; case SDLK_DOWN: ev = EV_DOWN; break;
                case SDLK_LEFT: ev = EV_LEFT; break; case SDLK_RIGHT: ev = EV_RIGHT; break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: ev = EV_ENTER; break; case SDLK_ESCAPE: ev = EV_ESC; break;
                }
                if (ev) res = launcher_event(ev, NULL, 0, 0);
            }
        }
        launcher_tick();
        launcher_draw(fb);
        SDL_UpdateTexture(t, NULL, fb, LAUNCHER_W * 4);
        SDL_RenderClear(r); SDL_RenderCopy(r, t, NULL, NULL); SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    SDL_DestroyTexture(t); SDL_DestroyRenderer(r); SDL_DestroyWindow(w);
    return res;
#else
    fprintf(stderr, "Este executável foi compilado sem janela (SDL2). Passe a ROM na linha de comando: ie3 <rom.nds>\n");
    return g_cfg.rom[0] ? LAUNCH_PLAY : LAUNCH_QUIT;
#endif
}

static void usage(const char *p) {
    fprintf(stderr,
        "uso: %s [rom.nds] [opções]\n"
        "  sem argumentos abre o launcher (escolha da ROM e configurações)\n"
        "  --jogar          pula o launcher e começa o jogo direto\n"
        "  --config <arq>   arquivo de configuração (padrão: ie3recomp.ini)\n"
        "  --fps <n>        limite de quadros (0 = sem limite)\n"
        "  --escala <n>     escala da janela (1 a 6)\n"
        "  --tela-cheia     começa em tela cheia\n"
        "  --game <rom>     a ROM (o mesmo que passar o caminho sozinho)\n"
        "  --data <pasta>   guarda tudo que o jogo escreve (save, .ini, log) nesta pasta\n"
        "  --sem-janela     roda sem mostrar nada (para testes; use RECOMP_DUMP para gravar quadros)\n"
        "  --trace          imprime cada função chamada (lento)\n", p);
}

int main(int argc, char **argv) {
    const char *rom_arg = NULL; int skip_launcher = 0;
    if (plat_self_exe(g_self_exe, sizeof g_self_exe)) snprintf(g_self_exe, sizeof g_self_exe, "%s", argv[0]);
    /* Pasta de dados: --data ou RECOMPLAUNCHER_DATA (RecompLauncher). Sem elas, tudo fica como antes. */
    const char *data_dir = getenv("RECOMPLAUNCHER_DATA"); int cfg_given = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) { g_cfg_path = argv[++i]; cfg_given = 1; }
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
    }
    static char data_cfg[1100], data_sav[1100], data_log[1100];
    if (data_dir && data_dir[0]) {
        plat_mkdir(data_dir);
        if (!cfg_given) { snprintf(data_cfg, sizeof data_cfg, "%s/ie3recomp.ini", data_dir); g_cfg_path = data_cfg; }
        extern const char *g_save_path;
        snprintf(data_sav, sizeof data_sav, "%s/ie3.sav", data_dir); g_save_path = data_sav;
        snprintf(data_log, sizeof data_log, "%s/logs", data_dir); plat_mkdir(data_log);
        snprintf(data_log, sizeof data_log, "%s/logs/ie3recomp.log", data_dir);
    }
    config_defaults(&g_cfg);
    config_load(&g_cfg, g_cfg_path);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--config") || !strcmp(a, "--data")) i++;
        else if (!strcmp(a, "--game") && i + 1 < argc) rom_arg = argv[++i];
        else if (!strcmp(a, "--trace")) g_trace = 1;
        else if (!strcmp(a, "--jogar")) skip_launcher = 1;
        else if (!strcmp(a, "--sem-janela")) { g_headless = 1; skip_launcher = 1; }
        else if (!strcmp(a, "--capturar-titulo") && i + 1 < argc) { g_capture_path = argv[++i]; g_headless = 1; skip_launcher = 1; g_rom_skip_sha = 1; }
        else if (!strcmp(a, "--tela-cheia")) g_cfg.fullscreen = 1;
        else if (!strcmp(a, "--fps") && i + 1 < argc) g_cfg.fps = atoi(argv[++i]);
        else if (!strcmp(a, "--escala") && i + 1 < argc) g_cfg.scale = atoi(argv[++i]);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "opção desconhecida: %s\n", a); usage(argv[0]); return 1; }
        else rom_arg = a;
    }
#ifndef HAVE_SDL2
    g_headless = 1;
#endif
    if (getenv("RECOMP_HEADLESS")) g_headless = 1;
    { extern int g_log_frames; g_log_frames = g_headless || getenv("RECOMP_QUADROS"); }
    if (!g_capture_path && data_log[0]) { if (freopen(data_log, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0); }
    else if (!g_capture_path) plat_log_to_file(g_self_exe, "ie3recomp.log"); /* Windows sem console: mensagens vão para um arquivo ao lado do .exe */
    { const char *mode = getenv("RECOMPLAUNCHER_MODE"); if (mode && !strcmp(mode, "bigpicture")) g_cfg.fullscreen = 1; }
    if (rom_arg) snprintf(g_cfg.rom, sizeof g_cfg.rom, "%s", rom_arg);
    if (!g_capture_path && (getenv("RECOMP_LAUNCHER_DUMP") || (!skip_launcher && !g_headless))) {
        if (run_launcher() != LAUNCH_PLAY) return 0;
    }
    if (!g_cfg.rom[0]) { usage(argv[0]); return 1; }

    char err[256];
    if (rom_open(g_cfg.rom, err, sizeof err)) { fprintf(stderr, "ROM: %s\n", err); return 2; } /* 2: não abriu a ROM */
    if (strcmp(g_romfile.sha1, "a70e9a336f6ea8ff78003938dcd024533f034a68"))
        fprintf(stderr, "aviso: SHA-1 %s diferente da versão esperada (BR T1.3); o código recompilado pode não bater com esta ROM\n", g_romfile.sha1);
    extern uint8_t *g_rom; extern size_t g_rom_size;
    g_rom = (uint8_t *)g_romfile.data; g_rom_size = g_romfile.size;

    /* ARM9 como está no cartão (comprimido): o crt0 recompilado descomprime e copia os autoloads (ITCM/DTCM),
     * do mesmo jeito que no aparelho. */
    memcpy(g_main + (g_romfile.arm9_ram & 0x3FFFFF), g_rom + g_romfile.arm9_off, g_romfile.arm9_size);

    /* área do sistema: cabeçalho da ROM em 02FFFE00, como o firmware deixa */
    memcpy(g_main + (0x02FFFE00 & 0x3FFFFF), g_rom, 0x170);
    put32(0x02FFFC00, 0x00001FC2); /* id do cartucho no boot; o SDK compara com o id lido (comando B8) */
    put32(0x02FFF800, 0x00001FC2);
    { uint16_t boot = 1; memcpy(g_main + (0x02FFFC40 & 0x3FFFFF), &boot, 2); } /* OS_BOOTTYPE_ROM: boot pelo cartão */
    { /* configurações do usuário do firmware (02FFFC80): calibração da tela de toque */
        uint8_t *u = g_main + (0x02FFFC80 & 0x3FFFFF);
        uint16_t ax1 = 0x200, ay1 = 0x200, ax2 = 0xE00, ay2 = 0x800;
        memcpy(u + 0x58, &ax1, 2); memcpy(u + 0x5A, &ay1, 2); u[0x5C] = 0x20; u[0x5D] = 0x20;
        memcpy(u + 0x5E, &ax2, 2); memcpy(u + 0x60, &ay2, 2); u[0x62] = 0xE0; u[0x63] = 0xA0;
    }
    put32(0x02FFFC00 + 0x388 + 4, 0xFFFFFFFFu); /* PXI: o ARM7 (HLE) já registrou todos os callbacks */

    Cpu *c = calloc(1, sizeof *c);
    c->mode = 0x13; c->I = 1; c->F = 1;
    c->r[13] = 0x02FFFC00; c->r[14] = 0xFFFF0000u;
    g_cpu = c;
#ifndef _WIN32
    if (g_capture_path) alarm(90); /* captura da arte: nunca fica pendurada (no Windows o launcher mata em 60 s) */
    if (getenv("RECOMP_WATCHDOG")) { signal(SIGALRM, on_alarm); alarm(atoi(getenv("RECOMP_WATCHDOG"))); }
    if (getenv("RECOMP_PROFILE")) {
        signal(SIGPROF, on_prof);
        struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, NULL);
    }
#endif
    rt_init_dispatch();
    fprintf(stderr, "[recomp] ROM ok (%s), iniciando _start em 02000800\n", g_romfile.sha1);
    c->rs[c->rsp++] = 0xFFFF0000u;
    f_arm9_02000800(c);
    fprintf(stderr, "[recomp] _start retornou\n");
    return 0;
}
