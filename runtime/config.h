#pragma once
/* Configuração do usuário, guardada em ie3recomp.ini ao lado do executável. */
typedef struct {
    char rom[1024];     /* caminho da ROM */
    int fps;            /* limite de quadros por segundo; 0 = sem limite */
    int scale;          /* escala da janela: 1 a 6 */
    int fullscreen;     /* 0/1 */
    int layout;         /* 0 vertical, 1 lado a lado, 2 só a tela de cima */
    int smooth;         /* 0 pixels nítidos, 1 filtro linear */
    int vsync;          /* 0/1 */
} Config;

extern Config g_cfg;
extern const char *g_cfg_path;
void config_defaults(Config *c);
int config_load(Config *c, const char *path);   /* 0 se leu o arquivo */
int config_save(const Config *c, const char *path);
