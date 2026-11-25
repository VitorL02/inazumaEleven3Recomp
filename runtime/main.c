/* Ponto de entrada do PC: monta a memória como o bootloader do DS deixaria e roda o crt0 recompilado. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include <signal.h>
#include <unistd.h>

static Cpu *g_cpu;
void rt_backtrace(Cpu *c);
/* RECOMP_PROFILE: amostra o endereço de retorno no topo da pilha a cada 1 ms */
#include <sys/time.h>
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
static void on_alarm(int sig) {
    (void)sig;
    fprintf(stderr, "\n[recomp] watchdog: travado. Último lr=%08x sp=%08x\n", g_cpu->r[14], g_cpu->r[13]);
    rt_backtrace(g_cpu);
    extern void rt_dump_threads(void);
    fprintf(stderr, "  threads:\n"); rt_dump_threads();
    if (getenv("RECOMP_PROFILE")) prof_dump();
    _exit(2);
}

void rt_init_dispatch(void);
void f_arm9_02000800(Cpu *c);  /* _start (crt0) */
const char *g_extracted = "extracted";
extern int g_trace;

uint8_t *load_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "não consegui abrir %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n ? n : 1);
    if (fread(d, 1, n, f) != (size_t)n) { fprintf(stderr, "erro lendo %s\n", path); exit(1); }
    fclose(f); *size = n; return d;
}
static void put32(uint32_t a, uint32_t v) { memcpy(g_main + (a & 0x3FFFFF), &v, 4); }

/* Vídeo: com SDL2 abre uma janela; sem SDL2 (ou com RECOMP_DUMP=<pasta>) grava quadros em PPM. */
#include "video.h"
extern uint64_t g_frame;
const uint8_t *rt_io(void);
static uint32_t g_fb[256 * 384];
#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
static SDL_Window *g_win; static SDL_Renderer *g_ren; static SDL_Texture *g_tex;
#endif
void video_present(void) {
    const char *dump = getenv("RECOMP_DUMP");
    int every = getenv("RECOMP_DUMP_EVERY") ? atoi(getenv("RECOMP_DUMP_EVERY")) : 60;
    int want_dump = dump && every > 0 && g_frame % every == 0;
#ifdef HAVE_SDL2
    int want_win = !getenv("RECOMP_HEADLESS");
#else
    int want_win = 0;
#endif
    if (!want_dump && !want_win) return;
    video_render(rt_io(), g_fb, g_fb + 256 * 192);
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
        if (!g_win) {
            SDL_Init(SDL_INIT_VIDEO);
            g_win = SDL_CreateWindow("Inazuma Eleven 3 Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 512, 768, SDL_WINDOW_RESIZABLE);
            g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_PRESENTVSYNC);
            g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 384);
        }
        SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) exit(0);
        SDL_UpdateTexture(g_tex, NULL, g_fb, 256 * 4);
        SDL_RenderClear(g_ren); SDL_RenderCopy(g_ren, g_tex, NULL, NULL); SDL_RenderPresent(g_ren);
    }
#endif
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <pasta extracted> <rom.nds> [--trace]\n", argv[0]); return 1; }
    g_extracted = argv[1];
    if (argc > 3 && !strcmp(argv[3], "--trace")) g_trace = 1;
    char p[512]; size_t n, n_itcm, n_dtcm, n_rom;
    snprintf(p, sizeof p, "%s/arm9.bin", g_extracted);  uint8_t *arm9 = load_file(p, &n);
    snprintf(p, sizeof p, "%s/autoload_01ff8000.bin", g_extracted); uint8_t *itcm = load_file(p, &n_itcm);
    snprintf(p, sizeof p, "%s/autoload_027e0000.bin", g_extracted); uint8_t *dtcm = load_file(p, &n_dtcm);
    uint8_t *rom = load_file(argv[2], &n_rom);
    extern uint8_t *g_rom; extern size_t g_rom_size;
    g_rom = rom; g_rom_size = n_rom;

    /* ARM9 já descomprimido + dados dos autoloads logo depois, + lista de autoload */
    memcpy(g_main, arm9, n);
    uint32_t a = 0x02000000 + (uint32_t)n;
    memcpy(g_main + (a & 0x3FFFFF), itcm, n_itcm); a += n_itcm;
    memcpy(g_main + (a & 0x3FFFFF), dtcm, n_dtcm); a += n_dtcm;
    uint32_t list = a;
    put32(list + 0, 0x01FF8000); put32(list + 4, (uint32_t)n_itcm); put32(list + 8, 0);
    put32(list + 12, 0x027E0000); put32(list + 16, (uint32_t)n_dtcm); put32(list + 20, 32);
    /* module params em 02000b88: zera compressed_static_end para o crt0 não descomprimir de novo */
    put32(0x02000b88 + 20, 0);
    if (*(uint32_t *)(g_main + 0xb88) != list) fprintf(stderr, "aviso: lista de autoload esperada em %08x\n", *(uint32_t *)(g_main + 0xb88));

    /* área do sistema: cabeçalho da ROM em 02FFFE00, como o firmware deixa */
    memcpy(g_main + (0x02FFFE00 & 0x3FFFFF), rom, 0x170);
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
    if (getenv("RECOMP_WATCHDOG")) { signal(SIGALRM, on_alarm); alarm(atoi(getenv("RECOMP_WATCHDOG"))); }
    if (getenv("RECOMP_PROFILE")) {
        signal(SIGPROF, on_prof);
        struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, NULL);
    }
    rt_init_dispatch();
    fprintf(stderr, "[recomp] iniciando _start em 02000800\n");
    c->rs[c->rsp++] = 0xFFFF0000u;
    f_arm9_02000800(c);
    fprintf(stderr, "[recomp] _start retornou\n");
    return 0;
}
