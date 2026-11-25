#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

Config g_cfg;
const char *g_cfg_path = "ie3recomp.ini";

void config_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    c->fps = 60; c->scale = 2; c->layout = 0; c->smooth = 0; c->vsync = 1;
}
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
}
int config_load(Config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *k = line, *v = eq + 1;
        trim(k); while (*v == ' ') v++; trim(v);
        if (!strcmp(k, "rom")) snprintf(c->rom, sizeof c->rom, "%s", v);
        else if (!strcmp(k, "fps")) c->fps = atoi(v);
        else if (!strcmp(k, "escala")) c->scale = atoi(v);
        else if (!strcmp(k, "tela_cheia")) c->fullscreen = atoi(v);
        else if (!strcmp(k, "layout")) c->layout = !strcmp(v, "lado_a_lado") ? 1 : !strcmp(v, "so_cima") ? 2 : 0;
        else if (!strcmp(k, "suavizar")) c->smooth = atoi(v);
        else if (!strcmp(k, "vsync")) c->vsync = atoi(v);
    }
    fclose(f);
    if (c->scale < 1) c->scale = 1; if (c->scale > 6) c->scale = 6;
    if (c->fps < 0) c->fps = 0;
    return 0;
}
int config_save(const Config *c, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    static const char *lay[] = {"vertical", "lado_a_lado", "so_cima"};
    fprintf(f, "# Inazuma Eleven 3 Recomp: configuração\n");
    fprintf(f, "rom = %s\n", c->rom);
    fprintf(f, "# quadros por segundo (0 = sem limite)\nfps = %d\n", c->fps);
    fprintf(f, "# escala da janela (1 a 6)\nescala = %d\n", c->scale);
    fprintf(f, "tela_cheia = %d\n", c->fullscreen);
    fprintf(f, "# vertical, lado_a_lado ou so_cima\nlayout = %s\n", lay[c->layout]);
    fprintf(f, "suavizar = %d\nvsync = %d\n", c->smooth, c->vsync);
    fclose(f);
    return 0;
}
