/* Estado da CPU ARM9 usado pelo código gerado. */
#pragma once
#include <stdint.h>

typedef struct Cpu {
    uint32_t r[16];
    uint32_t N, Z, C, V, Q;
    uint32_t mode;      /* bits de modo do CPSR (0x10..0x1f) */
    uint32_t I, F;      /* máscaras de IRQ/FIQ */
    uint32_t spsr[32];
    uint32_t bank_sp[32], bank_lr[32];
    /* pilha de retornos esperados: permite distinguir "retorno" de "salto" em rt_jump */
    uint32_t rs[4096];
    int rsp;
} Cpu;

typedef void (*GenFunc)(Cpu *c);
typedef struct { int module; uint32_t addr; GenFunc fn; } FuncEntry; /* module: -1 arm9, -2 itcm, >=0 overlay */
