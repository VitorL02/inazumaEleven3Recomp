#pragma once
#include <stddef.h>
#include <stdint.h>
/* O que muda entre Linux/macOS e Windows fica aqui. */
const uint8_t *plat_map_file(const char *path, size_t *size); /* arquivo inteiro só leitura; NULL se falhar */
void plat_unmap_file(const uint8_t *p, size_t size);
int plat_self_exe(char *out, size_t n);                        /* caminho do executável */
double plat_now(void);                                          /* segundos, relógio monotônico */
void plat_sleep(double sec);
/* processo filho sem janela e com stderr descartado; devolve um identificador (0 = falhou) */
intptr_t plat_spawn_quiet(const char *exe, const char *const argv[]);
int plat_proc_done(intptr_t h);  /* 1 se terminou (e libera), 0 se ainda roda */
void plat_proc_kill(intptr_t h);
