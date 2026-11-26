/* Janela do launcher (SDL2) e o modo de teste sem janela. Usado pelo ie3 e pelo launcher separado. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher.h"
#include "config.h"
#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#endif

static void write_ppm(const char *path, const uint32_t *fb, int w, int h) {
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t px[3] = {fb[i] >> 16, fb[i] >> 8, fb[i]}; fwrite(px, 1, 3, f); }
    fclose(f);
}

/* Launcher. Com SDL2 é interativo. Sem janela, RECOMP_LAUNCHER_DUMP=<arq.ppm> desenha a tela
 * (depois de aplicar RECOMP_LAUNCHER_KEYS="down,right,enter...") e sai: serve para teste. */
int launcher_run(const char *self_exe) {
    static uint32_t fb[LAUNCHER_W * LAUNCHER_H];
    launcher_init(self_exe);
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

