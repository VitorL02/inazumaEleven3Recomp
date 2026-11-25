/* Interpretador Thumb (ARMv5TE). O jogo tem só ~19 funções Thumb (SVC do SDK, matrizes),
 * então interpretar é mais simples que recompilar. Executa até voltar para o LR de entrada. */
#include <stdio.h>
#include "runtime.h"
#include "thumb.h"

void rt_fatal(Cpu *c, const char *msg, uint32_t a) __attribute__((noreturn));

static uint32_t lsl_c(Cpu *c, uint32_t v, uint32_t s) { uint32_t co; uint32_t r = sh_lsl_r(c, v, s, &co); c->C = co; return r; }
static uint32_t lsr_c(Cpu *c, uint32_t v, uint32_t s) { uint32_t co; uint32_t r = sh_lsr_r(c, v, s, &co); c->C = co; return r; }
static uint32_t asr_c(Cpu *c, uint32_t v, uint32_t s) { uint32_t co; uint32_t r = sh_asr_r(c, v, s, &co); c->C = co; return r; }
static uint32_t ror_c(Cpu *c, uint32_t v, uint32_t s) { uint32_t co; uint32_t r = sh_ror_r(c, v, s, &co); c->C = co; return r; }
static void nz(Cpu *c, uint32_t v) { c->N = v >> 31; c->Z = v == 0; }

static int cond_ok(Cpu *c, int cc) {
    switch (cc) {
    case 0: return c->Z; case 1: return !c->Z; case 2: return c->C; case 3: return !c->C;
    case 4: return c->N; case 5: return !c->N; case 6: return c->V; case 7: return !c->V;
    case 8: return c->C && !c->Z; case 9: return !c->C || c->Z; case 10: return c->N == c->V;
    case 11: return c->N != c->V; case 12: return !c->Z && c->N == c->V; case 13: return c->Z || c->N != c->V;
    }
    return 1;
}

/* Retorna 1 se a função terminou (voltou para ret). */
static int branch_to(Cpu *c, uint32_t t, uint32_t ret, uint32_t *pc) {
    if ((t & ~1u) == (ret & ~1u)) return 1;
    if (t & 1) { *pc = t & ~1u; return 0; }
    /* salto para ARM: chamada em cauda */
    c->r[14] = ret; rt_call(c, t); return 1;
}

void thumb_exec(Cpu *c, uint32_t pc) {
    uint32_t ret = c->r[14];
    for (;;) {
        uint32_t i = rd16(c, pc);
        uint32_t next = pc + 2;
        uint32_t *r = c->r;
        r[15] = pc + 4;
        switch (i >> 13) {
        case 0:
            if (((i >> 11) & 3) == 3) { /* add/sub */
                uint32_t rn = (i >> 3) & 7, rd = i & 7, v = (i >> 10) & 1 ? (i >> 6) & 7 : r[(i >> 6) & 7];
                r[rd] = (i >> 9) & 1 ? sub_f(c, r[rn], v, 1, 1) : add_f(c, r[rn], v, 0, 1);
            } else {
                uint32_t op = (i >> 11) & 3, imm = (i >> 6) & 31, rs = (i >> 3) & 7, rd = i & 7, v = r[rs];
                if (op == 0) r[rd] = imm ? lsl_c(c, v, imm) : v;
                else if (op == 1) r[rd] = lsr_c(c, v, imm ? imm : 32);
                else r[rd] = asr_c(c, v, imm ? imm : 32);
                nz(c, r[rd]);
            }
            break;
        case 1: {
            uint32_t op = (i >> 11) & 3, rd = (i >> 8) & 7, imm = i & 0xFF;
            if (op == 0) { r[rd] = imm; nz(c, imm); }
            else if (op == 1) sub_f(c, r[rd], imm, 1, 1);
            else if (op == 2) r[rd] = add_f(c, r[rd], imm, 0, 1);
            else r[rd] = sub_f(c, r[rd], imm, 1, 1);
            break; }
        case 2:
            if ((i >> 10) == 0x10) { /* ALU */
                uint32_t op = (i >> 6) & 15, rs = (i >> 3) & 7, rd = i & 7, a = r[rd], b = r[rs];
                switch (op) {
                case 0: r[rd] = a & b; nz(c, r[rd]); break;
                case 1: r[rd] = a ^ b; nz(c, r[rd]); break;
                case 2: r[rd] = lsl_c(c, a, b & 0xFF); nz(c, r[rd]); break;
                case 3: r[rd] = lsr_c(c, a, b & 0xFF); nz(c, r[rd]); break;
                case 4: r[rd] = asr_c(c, a, b & 0xFF); nz(c, r[rd]); break;
                case 5: r[rd] = add_f(c, a, b, c->C, 1); break;
                case 6: r[rd] = sub_f(c, a, b, c->C, 1); break;
                case 7: r[rd] = ror_c(c, a, b & 0xFF); nz(c, r[rd]); break;
                case 8: nz(c, a & b); break;
                case 9: r[rd] = sub_f(c, 0, b, 1, 1); break;
                case 10: sub_f(c, a, b, 1, 1); break;
                case 11: add_f(c, a, b, 0, 1); break;
                case 12: r[rd] = a | b; nz(c, r[rd]); break;
                case 13: r[rd] = a * b; nz(c, r[rd]); break;
                case 14: r[rd] = a & ~b; nz(c, r[rd]); break;
                case 15: r[rd] = ~b; nz(c, r[rd]); break;
                }
            } else if ((i >> 10) == 0x11) { /* hi regs / bx */
                uint32_t op = (i >> 8) & 3, rd = (i & 7) | ((i >> 4) & 8), rs = (i >> 3) & 15;
                uint32_t v = rs == 15 ? pc + 4 : r[rs];
                if (op == 0) { if (rd == 15) { if (branch_to(c, (r[rd] = pc + 4 + v) | 1, ret, &next)) return; } else r[rd] += v; }
                else if (op == 1) sub_f(c, rd == 15 ? pc + 4 : r[rd], v, 1, 1);
                else if (op == 2) { if (rd == 15) { if (branch_to(c, v | 1, ret, &next)) return; } else r[rd] = v; }
                else {
                    if ((i >> 7) & 1) { /* blx reg */
                        r[14] = next | 1; rt_call(c, v);
                    } else if (branch_to(c, v, ret, &next)) return;
                }
            } else if ((i >> 11) == 9) { /* ldr pc-relativo */
                r[(i >> 8) & 7] = rd32(c, ((pc + 4) & ~3u) + (i & 0xFF) * 4);
            } else { /* load/store com registrador */
                uint32_t ro = (i >> 6) & 7, rb = (i >> 3) & 7, rd = i & 7, a = r[rb] + r[ro];
                switch ((i >> 9) & 7) {
                case 0: wr32(c, a, r[rd]); break;
                case 1: wr16(c, a, r[rd]); break;
                case 2: wr8(c, a, r[rd]); break;
                case 3: r[rd] = (uint32_t)(int32_t)(int8_t)rd8(c, a); break;
                case 4: r[rd] = rd32r(c, a); break;
                case 5: r[rd] = rd16(c, a); break;
                case 6: r[rd] = rd8(c, a); break;
                case 7: r[rd] = (uint32_t)(int32_t)(int16_t)rd16(c, a); break;
                }
            }
            break;
        case 3: {
            uint32_t b = (i >> 12) & 1, l = (i >> 11) & 1, off = (i >> 6) & 31, rb = (i >> 3) & 7, rd = i & 7;
            uint32_t a = r[rb] + (b ? off : off * 4);
            if (l) r[rd] = b ? rd8(c, a) : rd32r(c, a); else if (b) wr8(c, a, r[rd]); else wr32(c, a, r[rd]);
            break; }
        case 4:
            if (!((i >> 12) & 1)) { /* ldrh/strh imediato */
                uint32_t a = r[(i >> 3) & 7] + ((i >> 6) & 31) * 2, rd = i & 7;
                if ((i >> 11) & 1) r[rd] = rd16(c, a); else wr16(c, a, r[rd]);
            } else { /* sp-relativo */
                uint32_t a = r[13] + (i & 0xFF) * 4, rd = (i >> 8) & 7;
                if ((i >> 11) & 1) r[rd] = rd32r(c, a); else wr32(c, a, r[rd]);
            }
            break;
        case 5:
            if (!((i >> 12) & 1)) { /* add rd, pc/sp, #imm */
                uint32_t rd = (i >> 8) & 7;
                r[rd] = ((i >> 11) & 1 ? r[13] : ((pc + 4) & ~3u)) + (i & 0xFF) * 4;
            } else if (((i >> 8) & 15) == 0) { /* add sp, #imm */
                uint32_t imm = (i & 0x7F) * 4; r[13] = (i >> 7) & 1 ? r[13] - imm : r[13] + imm;
            } else if (((i >> 9) & 3) == 2) { /* push/pop */
                uint32_t l = (i >> 11) & 1, extra = (i >> 8) & 1, list = i & 0xFF;
                int n = __builtin_popcount(list) + extra;
                if (!l) {
                    uint32_t a = r[13] - n * 4; r[13] = a;
                    for (int k = 0; k < 8; k++) if (list >> k & 1) { wr32(c, a, r[k]); a += 4; }
                    if (extra) wr32(c, a, r[14]);
                } else {
                    uint32_t a = r[13];
                    for (int k = 0; k < 8; k++) if (list >> k & 1) { r[k] = rd32(c, a); a += 4; }
                    uint32_t newpc = 0;
                    if (extra) { newpc = rd32(c, a); a += 4; }
                    r[13] = a;
                    if (extra && branch_to(c, newpc, ret, &next)) return;
                }
            } else if (((i >> 8) & 0xFF) == 0xBE) {
                rt_fatal(c, "BKPT em Thumb", pc);
            } else rt_fatal(c, "instrução Thumb desconhecida", (pc << 0) | 0);
            break;
        case 6:
            if (!((i >> 12) & 1)) { /* ldmia/stmia */
                uint32_t rb = (i >> 8) & 7, a = r[rb];
                for (int k = 0; k < 8; k++) if (i >> k & 1) {
                    if ((i >> 11) & 1) r[k] = rd32(c, a); else wr32(c, a, r[k]);
                    a += 4;
                }
                if (!((i >> 11) & 1) || !(i >> rb & 1)) r[rb] = a;
            } else {
                uint32_t cc = (i >> 8) & 15;
                if (cc == 15) { rt_swi(c, i & 0xFF); }
                else if (cc == 14) rt_fatal(c, "instrução Thumb indefinida", pc);
                else if (cond_ok(c, cc)) next = pc + 4 + ((int32_t)(int8_t)(i & 0xFF)) * 2;
            }
            break;
        case 7: {
            uint32_t h = (i >> 11) & 3;
            if (h == 0) next = pc + 4 + (((int32_t)(i << 21)) >> 20);
            else if (h == 2) r[14] = pc + 4 + (((int32_t)(i << 21)) >> 9);
            else { /* segunda metade de BL/BLX */
                uint32_t t = r[14] + ((i & 0x7FF) << 1);
                r[14] = next | 1;
                if (h == 3) rt_call(c, t | 1);
                else rt_call(c, t & ~3u);
            }
            break; }
        }
        pc = next;
    }
}
