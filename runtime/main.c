/* Ponto de entrada do PC: monta a memória como o bootloader do DS deixaria e roda o crt0 recompilado. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include <signal.h>
#include <unistd.h>

static Cpu *g_cpu;
void rt_backtrace(Cpu *c);
static void on_alarm(int sig) {
    (void)sig;
    fprintf(stderr, "\n[recomp] watchdog: travado. Último lr=%08x sp=%08x\n", g_cpu->r[14], g_cpu->r[13]);
    rt_backtrace(g_cpu);
    extern void rt_dump_threads(void);
    fprintf(stderr, "  threads:\n"); rt_dump_threads();
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

void video_present(void) { /* fase 2: ainda sem janela */ }

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
    put32(0x02FFFC00 + 0x388 + 4, 0xFFFFFFFFu); /* PXI: o ARM7 (HLE) já registrou todos os callbacks */

    Cpu *c = calloc(1, sizeof *c);
    c->mode = 0x13; c->I = 1; c->F = 1;
    c->r[13] = 0x02FFFC00; c->r[14] = 0xFFFF0000u;
    g_cpu = c;
    if (getenv("RECOMP_WATCHDOG")) { signal(SIGALRM, on_alarm); alarm(atoi(getenv("RECOMP_WATCHDOG"))); }
    rt_init_dispatch();
    fprintf(stderr, "[recomp] iniciando _start em 02000800\n");
    c->rs[c->rsp++] = 0xFFFF0000u;
    f_arm9_02000800(c);
    fprintf(stderr, "[recomp] _start retornou\n");
    return 0;
}
