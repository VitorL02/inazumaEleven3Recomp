/* Interpretador ARM (ARMv5TE) para o código que a tradução estática não cobre bem: o
 * decodificador Mobiclip do overlay 127, escrito à mão em assembly, com saltos calculados
 * (add pc, rX, rY) e "chamadas" montadas com add lr, pc. O DS roda o ARM9 a 67 MHz, então
 * interpretar esse trecho no PC é rápido o bastante.
 *
 * arm_exec(c, pc) roda a partir de pc até voltar para o LR de entrada. Chamadas para fora da
 * região interpretada vão para o código recompilado por rt_call. */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "thumb.h"
#include "arm_interp.h"

void rt_fatal(Cpu *c, const char *msg, uint32_t a) __attribute__((noreturn));

static int cond_ok(Cpu *c, uint32_t cc) {
    switch (cc) {
    case 0: return c->Z; case 1: return !c->Z; case 2: return c->C; case 3: return !c->C;
    case 4: return c->N; case 5: return !c->N; case 6: return c->V; case 7: return !c->V;
    case 8: return c->C && !c->Z; case 9: return !c->C || c->Z; case 10: return c->N == c->V;
    case 11: return c->N != c->V; case 12: return !c->Z && c->N == c->V; case 13: return c->Z || c->N != c->V;
    default: return 1;
    }
}
static inline void nz(Cpu *c, uint32_t v) { c->N = v >> 31; c->Z = v == 0; }

/* operando 2 com deslocamento imediato (bit 4 = 0) */
static uint32_t op2_shift_imm(Cpu *c, uint32_t i, uint32_t *co) {
    uint32_t v = c->r[i & 15], amt = (i >> 7) & 31;
    switch ((i >> 5) & 3) {
    case 0: if (!amt) { *co = c->C; return v; } *co = (v >> (32 - amt)) & 1; return v << amt;
    case 1: if (!amt) { *co = v >> 31; return 0; } *co = (v >> (amt - 1)) & 1; return v >> amt;
    case 2: if (!amt) { *co = v >> 31; return (uint32_t)((int32_t)v >> 31); } *co = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt);
    default:
        if (!amt) { *co = v & 1; return (c->C << 31) | (v >> 1); } /* RRX */
        *co = (v >> (amt - 1)) & 1; return (v >> amt) | (v << (32 - amt));
    }
}
/* operando 2 com deslocamento por registrador (bit 4 = 1); pc lido aqui vale +12 */
static uint32_t op2_shift_reg(Cpu *c, uint32_t i, uint32_t *co) {
    uint32_t rm = i & 15, v = rm == 15 ? c->r[15] + 4 : c->r[rm], s = c->r[(i >> 8) & 15] & 0xFF;
    switch ((i >> 5) & 3) {
    case 0: return sh_lsl_r(c, v, s, co);
    case 1: return sh_lsr_r(c, v, s, co);
    case 2: return sh_asr_r(c, v, s, co);
    default: return sh_ror_r(c, v, s, co);
    }
}
static inline int32_t sat_add(Cpu *c, int64_t v) {
    if (v > INT32_MAX) { c->Q = 1; return INT32_MAX; }
    if (v < INT32_MIN) { c->Q = 1; return INT32_MIN; }
    return (int32_t)v;
}
static inline int32_t half(uint32_t v, int top) { return top ? (int16_t)(v >> 16) : (int16_t)v; }

typedef enum { GO_ON, GO_DONE } Go;
/* Muda o pc. Devolve GO_DONE quando a função interpretada terminou (voltou para ret ou saiu
 * em chamada de cauda). */
static Go jump(Cpu *c, uint32_t t, uint32_t ret, uint32_t *pc) {
    if ((t & ~1u) == (ret & ~1u)) return GO_DONE;
    if (!(t & 1) && rt_interp_region(t)) { *pc = t; return GO_ON; }
    /* saída da região (ou para Thumb). Se o lr aponta para dentro da região, é uma chamada
     * montada à mão (add lr, pc / ldr pc, [...]): chama e continua no lr. */
    uint32_t lr = c->r[14];
    if (lr != ret && !(lr & 1) && rt_interp_region(lr)) { rt_call(c, t); *pc = lr; return GO_ON; }
    c->r[14] = ret; rt_call(c, t); /* chamada em cauda: o alvo volta direto para quem nos chamou */
    return GO_DONE;
}

void arm_exec(Cpu *c, uint32_t pc) {
    uint32_t ret = c->r[14];
    uint32_t *r = c->r;
    for (;;) {
        uint32_t i = rd32(c, pc);
        uint32_t next = pc + 4;
        r[15] = pc + 8;
        uint32_t cc = i >> 28;
        if (cc == 15) {
            if ((i & 0xFE000000u) == 0xFA000000u) { /* BLX imediato: chama Thumb */
                uint32_t t = pc + 8 + ((uint32_t)((int32_t)(i << 8) >> 6)) + ((i >> 23) & 2);
                r[14] = next; rt_call(c, t | 1);
            } else if ((i & 0xFD70F000u) == 0xF550F000u) { /* PLD */
            } else { fprintf(stderr, "[interp] instrução %08x em %08x\n", i, pc); rt_fatal(c, "interpretador ARM: instrução não suportada em", pc); }
            pc = next; continue;
        }
        if (!cond_ok(c, cc)) { pc = next; continue; }

        uint32_t top = (i >> 25) & 7;
        if (top <= 1) {
            /* ---- instruções diversas no espaço de processamento de dados ---- */
            if (top == 0) {
                if ((i & 0x0FFFFFD0u) == 0x012FFF10u) { /* BX / BLX registrador */
                    uint32_t t = r[i & 15];
                    if (i & 0x20) { r[14] = next; rt_call(c, t); pc = next; continue; }
                    if (jump(c, t, ret, &pc) == GO_DONE) return;
                    continue;
                }
                if ((i & 0x0FFF0FF0u) == 0x016F0F10u) { uint32_t v = r[i & 15]; r[(i >> 12) & 15] = v ? (uint32_t)__builtin_clz(v) : 32; pc = next; continue; }
                if ((i & 0x0F900FF0u) == 0x01000050u) { /* QADD/QSUB/QDADD/QDSUB */
                    int32_t a = (int32_t)r[i & 15], b = (int32_t)r[(i >> 16) & 15];
                    uint32_t op = (i >> 21) & 3;
                    if (op & 2) b = sat_add(c, (int64_t)b * 2);
                    r[(i >> 12) & 15] = (uint32_t)sat_add(c, (op & 1) ? (int64_t)a - b : (int64_t)a + b);
                    pc = next; continue;
                }
                if ((i & 0x0F900090u) == 0x01000080u) { /* SMLAxy / SMLAWy / SMULWy / SMLALxy / SMULxy */
                    uint32_t op = (i >> 21) & 3, rd = (i >> 16) & 15, rn = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
                    int x = (i >> 5) & 1, y = (i >> 6) & 1;
                    if (op == 0) { int64_t p = (int64_t)half(r[rm], x) * half(r[rs], y); r[rd] = (uint32_t)sat_add(c, p + (int32_t)r[rn]); if ((int64_t)(int32_t)(uint32_t)(p + (int32_t)r[rn]) != p + (int32_t)r[rn]) c->Q = 1; }
                    else if (op == 1) {
                        int64_t p = ((int64_t)(int32_t)r[rm] * half(r[rs], y)) >> 16;
                        if (x) r[rd] = (uint32_t)(int32_t)p; /* SMULWy */
                        else { int64_t s = p + (int32_t)r[rn]; if (s != (int32_t)s) c->Q = 1; r[rd] = (uint32_t)s; } /* SMLAWy */
                    } else if (op == 2) {
                        int64_t acc = (int64_t)(((uint64_t)r[rd] << 32) | r[rn]);
                        acc += (int64_t)half(r[rm], x) * half(r[rs], y);
                        r[rn] = (uint32_t)acc; r[rd] = (uint32_t)((uint64_t)acc >> 32);
                    } else r[rd] = (uint32_t)(half(r[rm], x) * half(r[rs], y));
                    pc = next; continue;
                }
                if ((i & 0x0FC000F0u) == 0x00000090u) { /* MUL / MLA */
                    uint32_t rd = (i >> 16) & 15, v = r[i & 15] * r[(i >> 8) & 15];
                    if (i & (1u << 21)) v += r[(i >> 12) & 15];
                    r[rd] = v; if (i & (1u << 20)) nz(c, v);
                    pc = next; continue;
                }
                if ((i & 0x0F8000F0u) == 0x00800090u) { /* UMULL/UMLAL/SMULL/SMLAL */
                    uint32_t hi = (i >> 16) & 15, lo = (i >> 12) & 15, a = r[i & 15], b = r[(i >> 8) & 15];
                    uint64_t v = (i & (1u << 22)) ? (uint64_t)((int64_t)(int32_t)a * (int32_t)b) : (uint64_t)a * b;
                    if (i & (1u << 21)) v += ((uint64_t)r[hi] << 32) | r[lo];
                    r[lo] = (uint32_t)v; r[hi] = (uint32_t)(v >> 32);
                    if (i & (1u << 20)) { c->N = (uint32_t)(v >> 63); c->Z = v == 0; }
                    pc = next; continue;
                }
                if ((i & 0x0FB00FF0u) == 0x01000090u) { /* SWP / SWPB */
                    uint32_t a = r[(i >> 16) & 15], v = r[i & 15];
                    if (i & (1u << 22)) { uint32_t o = rd8(c, a); wr8(c, a, v); r[(i >> 12) & 15] = o; }
                    else { uint32_t o = rd32r(c, a); wr32(c, a, v); r[(i >> 12) & 15] = o; }
                    pc = next; continue;
                }
                if ((i & 0x0E000090u) == 0x00000090u && (i & 0x60)) { /* LDRH/STRH/LDRSB/LDRSH/LDRD/STRD */
                    uint32_t P = (i >> 24) & 1, U = (i >> 23) & 1, W = (i >> 21) & 1, L = (i >> 20) & 1;
                    uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15, sh = (i >> 5) & 3;
                    uint32_t off = (i & (1u << 22)) ? (((i >> 4) & 0xF0) | (i & 15)) : r[i & 15];
                    uint32_t base = r[rn], a = P ? (U ? base + off : base - off) : base;
                    uint32_t wb = U ? base + off : base - off;
                    if (L) {
                        uint32_t v = sh == 1 ? rd16(c, a) : sh == 2 ? (uint32_t)(int32_t)(int8_t)rd8(c, a) : (uint32_t)(int32_t)(int16_t)rd16(c, a);
                        if (!P || W) r[rn] = wb;
                        r[rd] = v;
                    } else if (sh == 1) { wr16(c, a, r[rd]); if (!P || W) r[rn] = wb; }
                    else if (sh == 2) { uint32_t v0 = rd32(c, a), v1 = rd32(c, a + 4); if (!P || W) r[rn] = wb; r[rd] = v0; r[rd + 1] = v1; } /* LDRD */
                    else { wr32(c, a, r[rd]); wr32(c, a + 4, r[rd + 1]); if (!P || W) r[rn] = wb; } /* STRD */
                    pc = next; continue;
                }
                if ((i & 0x0FB000F0u) == 0x01000000u) { /* MRS */
                    r[(i >> 12) & 15] = (i & (1u << 22)) ? rt_get_spsr(c) : rt_get_cpsr(c);
                    pc = next; continue;
                }
                if ((i & 0x0FB0FFF0u) == 0x0120F000u) { /* MSR registrador */
                    uint32_t mask = (i >> 16) & 15;
                    if (i & (1u << 22)) rt_set_spsr(c, r[i & 15], mask); else rt_set_cpsr(c, r[i & 15], mask);
                    pc = next; continue;
                }
            } else if ((i & 0x0FB0F000u) == 0x0320F000u) { /* MSR imediato */
                uint32_t rot = ((i >> 8) & 15) * 2, v = i & 0xFF; v = rot ? (v >> rot) | (v << (32 - rot)) : v;
                uint32_t mask = (i >> 16) & 15;
                if (i & (1u << 22)) rt_set_spsr(c, v, mask); else rt_set_cpsr(c, v, mask);
                pc = next; continue;
            }
            /* ---- processamento de dados ---- */
            uint32_t op = (i >> 21) & 15, S = (i >> 20) & 1, rn = (i >> 16) & 15, rd = (i >> 12) & 15;
            uint32_t co = c->C, b;
            if (top == 1) {
                uint32_t rot = ((i >> 8) & 15) * 2; b = i & 0xFF;
                if (rot) { b = (b >> rot) | (b << (32 - rot)); co = b >> 31; }
            } else if (i & 0x10) b = op2_shift_reg(c, i, &co);
            else b = op2_shift_imm(c, i, &co);
            uint32_t a = (rn == 15 && top == 0 && (i & 0x10)) ? r[15] + 4 : r[rn], v = 0;
            int logic = 0, write = 1;
            switch (op) {
            case 0: v = a & b; logic = 1; break;
            case 1: v = a ^ b; logic = 1; break;
            case 2: v = sub_f(c, a, b, 1, S); break;
            case 3: v = sub_f(c, b, a, 1, S); break;
            case 4: v = add_f(c, a, b, 0, S); break;
            case 5: v = add_f(c, a, b, c->C, S); break;
            case 6: v = sub_f(c, a, b, c->C, S); break;
            case 7: v = sub_f(c, b, a, c->C, S); break;
            case 8: v = a & b; logic = 1; write = 0; break;
            case 9: v = a ^ b; logic = 1; write = 0; break;
            case 10: sub_f(c, a, b, 1, 1); write = 0; break;
            case 11: add_f(c, a, b, 0, 1); write = 0; break;
            case 12: v = a | b; logic = 1; break;
            case 13: v = b; logic = 1; break;
            case 14: v = a & ~b; logic = 1; break;
            case 15: v = ~b; logic = 1; break;
            }
            if (logic && S) { nz(c, v); c->C = co; }
            if (write) {
                if (rd == 15) {
                    if (S) rt_fatal(c, "interpretador ARM: retorno de exceção em", pc);
                    if (jump(c, v, ret, &pc) == GO_DONE) return;
                    continue;
                }
                r[rd] = v;
            }
            pc = next; continue;
        }
        if (top == 2 || top == 3) { /* LDR/STR/LDRB/STRB */
            if (top == 3 && (i & 0x10)) { fprintf(stderr, "[interp] instrução %08x em %08x\n", i, pc); rt_fatal(c, "interpretador ARM: instrução indefinida em", pc); }
            uint32_t P = (i >> 24) & 1, U = (i >> 23) & 1, B = (i >> 22) & 1, W = (i >> 21) & 1, L = (i >> 20) & 1;
            uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15, co;
            uint32_t off = top == 2 ? (i & 0xFFF) : op2_shift_imm(c, i, &co);
            uint32_t base = r[rn], a = P ? (U ? base + off : base - off) : base;
            uint32_t wb = U ? base + off : base - off;
            if (L) {
                uint32_t v = B ? rd8(c, a) : rd32r(c, a);
                if (!P || W) r[rn] = wb;
                if (rd == 15) { if (jump(c, v, ret, &pc) == GO_DONE) return; continue; }
                r[rd] = v;
            } else {
                uint32_t v = rd == 15 ? r[15] + 4 : r[rd];
                if (B) wr8(c, a, v); else wr32(c, a, v);
                if (!P || W) r[rn] = wb;
            }
            pc = next; continue;
        }
        if (top == 4) { /* LDM/STM */
            uint32_t P = (i >> 24) & 1, U = (i >> 23) & 1, W = (i >> 21) & 1, L = (i >> 20) & 1;
            uint32_t rn = (i >> 16) & 15, list = i & 0xFFFF, n = (uint32_t)__builtin_popcount(list);
            uint32_t base = r[rn];
            uint32_t a = U ? (P ? base + 4 : base) : (P ? base - 4 * n : base - 4 * n + 4);
            uint32_t wb = U ? base + 4 * n : base - 4 * n;
            if (L) {
                uint32_t vals[16], newpc = 0; int haspc = 0;
                for (int k = 0; k < 16; k++) if (list & (1u << k)) { vals[k] = rd32(c, a); a += 4; }
                if (W && !(list & (1u << rn))) r[rn] = wb;
                for (int k = 0; k < 15; k++) if (list & (1u << k)) r[k] = vals[k];
                if (list & 0x8000) { haspc = 1; newpc = vals[15]; }
                if (haspc) { if (jump(c, newpc, ret, &pc) == GO_DONE) return; continue; }
            } else {
                for (int k = 0; k < 16; k++) if (list & (1u << k)) { wr32(c, a, k == 15 ? r[15] + 4 : (k == (int)rn ? base : r[k])); a += 4; }
                if (W) r[rn] = wb;
            }
            pc = next; continue;
        }
        if (top == 5) { /* B / BL */
            uint32_t t = pc + 8 + (uint32_t)((int32_t)(i << 8) >> 6);
            if (i & (1u << 24)) { /* BL */
                r[14] = next;
                if (rt_interp_region(t)) { pc = t; continue; }
                rt_call(c, t); pc = next; continue;
            }
            if (jump(c, t, ret, &pc) == GO_DONE) return;
            continue;
        }
        if ((i & 0x0F000000u) == 0x0F000000u) { rt_swi(c, (i >> 16) & 0xFF); pc = next; continue; }
        if ((i & 0x0F000010u) == 0x0E000010u) { /* MRC/MCR */
            int cp = (i >> 8) & 15, op1 = (i >> 21) & 7, crn = (i >> 16) & 15, crm = i & 15, op2 = (i >> 5) & 7;
            uint32_t rd = (i >> 12) & 15;
            if (i & (1u << 20)) { uint32_t v = rt_mrc(c, cp, op1, crn, crm, op2); if (rd != 15) r[rd] = v; }
            else rt_mcr(c, cp, op1, crn, crm, op2, r[rd]);
            pc = next; continue;
        }
        fprintf(stderr, "[interp] instrução %08x em %08x\n", i, pc);
        rt_fatal(c, "interpretador ARM: instrução não suportada em", pc);
    }
}
