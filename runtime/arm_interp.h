#pragma once
#include "cpu.h"
/* interpreta ARM a partir de pc até voltar para o LR de entrada (runtime/arm_interp.c) */
void arm_exec(Cpu *c, uint32_t pc);
/* 1 se o endereço está numa região que roda no interpretador (overlay 127 carregado) */
int rt_interp_region(uint32_t a);
