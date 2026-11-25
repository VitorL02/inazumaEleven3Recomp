/* Funções do runtime chamadas pelo código gerado. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "cpu.h"

extern uint8_t g_main[4 << 20];
extern uint8_t g_itcm[32 << 10];
extern uint8_t g_dtcm[16 << 10];
#define DTCM_BASE 0x027E0000u

uint32_t mem_rd32_slow(Cpu *c, uint32_t a);
uint32_t mem_rd16_slow(Cpu *c, uint32_t a);
uint32_t mem_rd8_slow(Cpu *c, uint32_t a);
void mem_wr32_slow(Cpu *c, uint32_t a, uint32_t v);
void mem_wr16_slow(Cpu *c, uint32_t a, uint32_t v);
void mem_wr8_slow(Cpu *c, uint32_t a, uint32_t v);

static inline int is_main(uint32_t a) { return (a >> 24) == 2 && (a - DTCM_BASE) >= 0x4000u; }

static inline uint32_t rd32(Cpu *c, uint32_t a) {
    a &= ~3u;
    if (is_main(a)) { uint32_t v; memcpy(&v, g_main + (a & 0x3FFFFF), 4); return v; }
    return mem_rd32_slow(c, a);
}
static inline uint32_t rd32r(Cpu *c, uint32_t a) { /* LDR desalinhado rotaciona */
    uint32_t v = rd32(c, a), s = (a & 3) * 8;
    return s ? (v >> s) | (v << (32 - s)) : v;
}
static inline uint32_t rd16(Cpu *c, uint32_t a) {
    a &= ~1u;
    if (is_main(a)) { uint16_t v; memcpy(&v, g_main + (a & 0x3FFFFF), 2); return v; }
    return mem_rd16_slow(c, a);
}
static inline uint32_t rd8(Cpu *c, uint32_t a) {
    if (is_main(a)) return g_main[a & 0x3FFFFF];
    return mem_rd8_slow(c, a);
}
static inline void wr32(Cpu *c, uint32_t a, uint32_t v) {
    a &= ~3u;
    if (is_main(a)) { memcpy(g_main + (a & 0x3FFFFF), &v, 4); return; }
    mem_wr32_slow(c, a, v);
}
static inline void wr16(Cpu *c, uint32_t a, uint32_t v) {
    a &= ~1u;
    if (is_main(a)) { uint16_t h = (uint16_t)v; memcpy(g_main + (a & 0x3FFFFF), &h, 2); return; }
    mem_wr16_slow(c, a, v);
}
static inline void wr8(Cpu *c, uint32_t a, uint32_t v) {
    if (is_main(a)) { g_main[a & 0x3FFFFF] = (uint8_t)v; return; }
    mem_wr8_slow(c, a, v);
}

/* aritmética com flags */
static inline uint32_t add_f(Cpu *c, uint32_t a, uint32_t b, uint32_t cin, int s) {
    uint64_t r = (uint64_t)a + b + cin;
    uint32_t r32 = (uint32_t)r;
    if (s) { c->N = r32 >> 31; c->Z = r32 == 0; c->C = (uint32_t)(r >> 32); c->V = ((~(a ^ b) & (a ^ r32)) >> 31); }
    return r32;
}
static inline uint32_t sub_f(Cpu *c, uint32_t a, uint32_t b, uint32_t cin, int s) {
    /* a - b - !cin */
    uint64_t r = (uint64_t)a + (uint32_t)~b + cin;
    uint32_t r32 = (uint32_t)r;
    if (s) { c->N = r32 >> 31; c->Z = r32 == 0; c->C = (uint32_t)(r >> 32); c->V = (((a ^ b) & (a ^ r32)) >> 31); }
    return r32;
}
static inline uint32_t sh_lsl_r(Cpu *c, uint32_t v, uint32_t s, uint32_t *co) {
    if (s == 0) { *co = c->C; return v; }
    if (s < 32) { *co = (v >> (32 - s)) & 1; return v << s; }
    *co = s == 32 ? (v & 1) : 0; return 0;
}
static inline uint32_t sh_lsr_r(Cpu *c, uint32_t v, uint32_t s, uint32_t *co) {
    if (s == 0) { *co = c->C; return v; }
    if (s < 32) { *co = (v >> (s - 1)) & 1; return v >> s; }
    *co = s == 32 ? (v >> 31) : 0; return 0;
}
static inline uint32_t sh_asr_r(Cpu *c, uint32_t v, uint32_t s, uint32_t *co) {
    if (s == 0) { *co = c->C; return v; }
    if (s < 32) { *co = (v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    *co = v >> 31; return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t sh_ror_r(Cpu *c, uint32_t v, uint32_t s, uint32_t *co) {
    if (s == 0) { *co = c->C; return v; }
    s &= 31;
    if (s == 0) { *co = v >> 31; return v; }
    *co = (v >> (s - 1)) & 1; return (v >> s) | (v << (32 - s));
}

/* controle de fluxo */
void rt_call(Cpu *c, uint32_t target);           /* chamada indireta (bit 0 = Thumb) */
void rt_jump(Cpu *c, uint32_t target);           /* salto indireto: retorno ou chamada em cauda */
void rt_fallthrough(Cpu *c, uint32_t next);      /* código continua numa função vizinha */
void rt_unimpl(Cpu *c, uint32_t addr, uint32_t insn);
void rt_exception_return(Cpu *c, uint32_t target);
void rt_swi(Cpu *c, uint32_t n);
uint32_t rt_mrc(Cpu *c, int cp, int op1, int crn, int crm, int op2);
void rt_mcr(Cpu *c, int cp, int op1, int crn, int crm, int op2, uint32_t v);
uint32_t rt_get_cpsr(Cpu *c);
uint32_t rt_get_spsr(Cpu *c);
void rt_set_cpsr(Cpu *c, uint32_t v, int mask);
void rt_set_spsr(Cpu *c, uint32_t v, int mask);

#define CALL(c, ret, fn) do { (c)->r[14] = (ret); (c)->rs[(c)->rsp++] = (ret); fn(c); (c)->rsp--; } while (0)
