#pragma once
#include <stdint.h>
/* Launcher: tela inicial com a escolha da ROM e as opções, no estilo dos launchers de recomp.
 * Desenha num buffer de pixels (ARGB); a plataforma (SDL2) só mostra o buffer e entrega os eventos,
 * então dá para testar sem janela (RECOMP_LAUNCHER_DUMP). */
#define LAUNCHER_W 960
#define LAUNCHER_H 540
enum { EV_NONE, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_ENTER, EV_ESC, EV_DROP, EV_CLICK, EV_MOTION };
enum { LAUNCH_CONTINUE, LAUNCH_PLAY, LAUNCH_QUIT };
void launcher_init(const char *self_exe);
int launcher_event(int ev, const char *path, int mx, int my); /* devolve LAUNCH_* */
void launcher_tick(void);          /* chamado a cada quadro: acompanha a captura da arte da ROM */
void launcher_wait_art(int secs);  /* para testes: espera a captura terminar */
void launcher_draw(uint32_t *fb);
