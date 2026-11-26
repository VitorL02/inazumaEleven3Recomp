/* "Inazuma Eleven 3.exe": o launcher da release, sozinho, sem nenhum código do jogo.
 * Na primeira vez ele gera o jogo a partir da ROM do usuário (preparo, sem janela de console) em
 * <pasta>/jogo; depois o botão Jogar abre <pasta>/jogo/ie3 e o launcher fecha.
 * As opções (ie3recomp.ini) e o save (ie3.sav) ficam na pasta do launcher. */
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "launcher.h"
#include "config.h"

int main(int argc, char **argv) {
    (void)argc;
    static char self[1024], base[1024], cfg[1100];
    if (plat_self_exe(self, sizeof self)) snprintf(self, sizeof self, "%s", argv[0]);
    snprintf(base, sizeof base, "%s", self);
    char *s = strrchr(base, '/'), *s2 = strrchr(base, '\\');
    if (s2 > s) s = s2;
    if (s) *s = 0; else snprintf(base, sizeof base, ".");
#ifdef _WIN32
    snprintf(cfg, sizeof cfg, "%s\\ie3recomp.ini", base);
#else
    snprintf(cfg, sizeof cfg, "%s/ie3recomp.ini", base);
#endif
    g_cfg_path = cfg;
    plat_log_to_file(self, "launcher.log");
    config_defaults(&g_cfg);
    config_load(&g_cfg, g_cfg_path);
    launcher_set_external(base);
    launcher_run(self);
    return 0;
}
