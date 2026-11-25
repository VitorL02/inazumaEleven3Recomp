#pragma once
#include <stddef.h>
/* Contextos do host para as fibras (threads do SDK e tarefas do jogo):
 * ucontext no Linux/macOS e Fibers do Win32 no Windows. */
#ifdef _WIN32
typedef struct { void *fib; void (*fn)(void); } HostCtx;
#else
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include <ucontext.h>
typedef struct { ucontext_t uc; void *stack; } HostCtx;
#endif
/* prepara `h` para começar em fn() na próxima troca (reaproveita a pilha se já houver) */
void hostctx_prepare(HostCtx *h, void (*fn)(void), size_t stack);
/* salva o contexto atual em `from` e continua `to` */
void hostctx_swap(HostCtx *from, HostCtx *to);
