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
/* Windows sem console (build de janela): manda stderr para <pasta do exe>/<nome>. Nos outros, não faz nada. */
void plat_log_to_file(const char *self_exe, const char *name);
/* cria a pasta (só o último nível); 0 se criou ou já existia */
int plat_mkdir(const char *path);
/* --- launcher separado (o jogo é gerado da ROM pelo preparo e roda como outro processo) --- */
/* processo sem janela, com stdout e stderr num arquivo; termina junto com este processo
 * (no Windows fica num "job" que o sistema encerra quando o launcher fecha) */
intptr_t plat_spawn_logged(const char *exe, const char *const argv[], const char *log, const char *cwd);
/* processo independente (o jogo), com a pasta de trabalho cwd; 0 se abriu */
int plat_spawn_detached(const char *exe, const char *const argv[], const char *cwd);
int plat_file_exists(const char *path);
/* janela "abrir arquivo" do sistema para escolher a ROM; 0 se o usuário escolheu um arquivo */
int plat_pick_file(char *out, size_t n);
