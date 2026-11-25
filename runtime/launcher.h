#pragma once
#include <stdint.h>
/* Launcher: tela inicial com a escolha da ROM e as opções. Desenha num buffer de pixels;
 * a plataforma (SDL2) só mostra o buffer e entrega os eventos. */
#define LAUNCHER_W 512
#define LAUNCHER_H 384
enum { EV_NONE, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_ENTER, EV_ESC, EV_DROP, EV_CLICK };
enum { LAUNCH_CONTINUE, LAUNCH_PLAY, LAUNCH_QUIT };
void launcher_init(void);
int launcher_event(int ev, const char *path, int mx, int my); /* devolve LAUNCH_* */
void launcher_draw(uint32_t *fb);
void draw_text(uint32_t *fb, int fbw, int fbh, int x, int y, const char *utf8, uint32_t color);
