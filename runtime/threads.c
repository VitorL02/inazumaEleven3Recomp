/* Threads do NitroSDK (OSThread) mapeadas em fibras do host (ucontext).
 *
 * No DS, a troca de thread é feita por OS_SaveContext/OS_LoadContext, que funcionam
 * como setjmp/longjmp sobre os registradores do ARM. Em código recompilado a pilha
 * do C também precisa ser trocada, então cada thread do jogo ganha uma fibra do host.
 * A função OSi_RescheduleThread (02001728) é substituída inteira por hle_OSi_RescheduleThread.
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include "runtime.h"

void rt_fatal(Cpu *c, const char *msg, uint32_t a) __attribute__((noreturn));

#define MAX_FIBERS 64
#define FIBER_STACK (8 << 20)
#define TI_BASE 0x02093988u  /* OSi_ThreadInfo (literal em 020017e4) */
#define TI_FLAGS 0x020939a4u /* isNeedRescheduling / irqDepth (literal em 020017e8) */

typedef struct {
    uint32_t ctx;     /* endereço do OSContext (= OSThread) no DS */
    ucontext_t uc;
    Cpu saved;        /* estado da CPU quando a fibra foi suspensa */
    int used;
} Fiber;

static Fiber g_fibers[MAX_FIBERS];
static Fiber *g_cur;
static Cpu *g_cpu_main;

static Fiber *fiber_for(uint32_t ctx) {
    for (int i = 0; i < MAX_FIBERS; i++) if (g_fibers[i].used && g_fibers[i].ctx == ctx) return &g_fibers[i];
    return NULL;
}
static Fiber *fiber_new(uint32_t ctx) {
    for (int i = 0; i < MAX_FIBERS; i++) if (!g_fibers[i].used) { memset(&g_fibers[i], 0, sizeof g_fibers[i]); g_fibers[i].used = 1; g_fibers[i].ctx = ctx; return &g_fibers[i]; }
    rt_fatal(g_cpu_main, "fibras demais; ctx", ctx);
}

/* Primeira execução de uma thread nova: lê registradores do OSContext e chama a função de entrada. */
static void fiber_entry(void) {
    Cpu *c = g_cpu_main;
    uint32_t ctx = g_cur->ctx;
    uint32_t cpsr = rd32(c, ctx);
    for (int i = 0; i < 15; i++) c->r[i] = rd32(c, ctx + 4 + i * 4);
    uint32_t pc = rd32(c, ctx + 0x40) - 4;
    c->mode = cpsr & 0x1F; c->I = (cpsr >> 7) & 1;
    c->rsp = 0;
    uint32_t exit_lr = c->r[14];
    fprintf(stderr, "[recomp] thread nova %08x começa em %08x (sp=%08x)\n", ctx, pc, c->r[13]);
    rt_call(c, pc | ((cpsr >> 5) & 1));
    c->r[14] = 0; rt_call(c, exit_lr); /* OS_ExitThread */
    rt_fatal(c, "thread terminou sem trocar de contexto", ctx);
}

/* Suspende a fibra atual (ligada a `from`, 0 se não for voltar) e continua `to`. */
static void fiber_switch(Cpu *c, uint32_t from, uint32_t to) {
    if (!g_cur) { g_cur = fiber_new(from); }   /* fibra principal (thread que chamou primeiro) */
    else if (from) g_cur->ctx = from;
    Fiber *me = g_cur;
    Fiber *next = fiber_for(to);
    if (next == me) return;
    me->saved = *c;
    if (!next) {
        next = fiber_new(to);
        getcontext(&next->uc);
        next->uc.uc_stack.ss_sp = malloc(FIBER_STACK);
        next->uc.uc_stack.ss_size = FIBER_STACK;
        next->uc.uc_link = NULL;
        makecontext(&next->uc, fiber_entry, 0);
        g_cur = next;
        swapcontext(&me->uc, &next->uc);
    } else {
        g_cur = next;
        *c = next->saved;
        swapcontext(&me->uc, &next->uc);
    }
    /* voltamos: alguém trocou para nós */
    *c = me->saved;
    if (!from) rt_fatal(c, "thread terminada foi retomada", me->ctx);
}

/* OSi_RescheduleThread (02001728), reescrita com fibras */
void hle_OSi_RescheduleThread(Cpu *c) {
    g_cpu_main = c;
    if (rd32(c, TI_BASE + 4)) return;
    if (rd16(c, TI_FLAGS + 2) || c->mode == 0x12) { wr16(c, TI_FLAGS, 1); return; }
    uint32_t cur = rd32(c, rd32(c, TI_BASE + 8));
    uint32_t r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], lr = c->r[14];
    c->r[14] = 0x02001770u; rt_call(c, 0x02001d08u); /* OS_SelectThread */
    uint32_t next = c->r[0];
    if (cur == next || !next) { c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[14] = lr; return; }
    int dead = rd32(c, cur + 0x64) == 2;
    uint32_t cb = rd32(c, TI_BASE);
    if (cb) { c->r[0] = cur; c->r[1] = next; c->r[14] = 0x020017b8u; rt_call(c, cb); }
    cb = rd32(c, TI_FLAGS + 0xC);
    if (cb) { c->r[0] = cur; c->r[1] = next; c->r[14] = 0x020017d0u; rt_call(c, cb); }
    wr32(c, TI_BASE + 0x20, next);
    c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[14] = lr;
    fiber_switch(c, dead ? 0 : cur, next);
}

/* OS_LoadContext (020020cc) chamado fora do reschedule (ex.: OS_ExitThread): troca sem voltar. */
void hle_OS_LoadContext(Cpu *c) {
    g_cpu_main = c;
    fiber_switch(c, 0, c->r[0]);
}

/* Depois de rodar handlers de IRQ: faz a troca pendente (o OS_IrqHandler_ThreadSwitch do ITCM). */
void rt_after_irq(Cpu *c) {
    /* acorda as threads que esperam IRQ (OSi_IrqThreadQueue em 027E0060), como o OS_IrqHandler do ITCM */
    uint32_t q = 0x027E0060u, t = rd32(c, q);
    if (t) {
        while (t) {
            wr32(c, t + 0x64, 1); wr32(c, t + 0x78, 0); wr32(c, t + 0x7C, 0);
            uint32_t n = rd32(c, t + 0x80); wr32(c, t + 0x80, 0); t = n;
        }
        wr32(c, q, 0); wr32(c, q + 4, 0);
        wr16(c, TI_FLAGS, 1);
    }
    if (rd16(c, TI_FLAGS)) {
        wr16(c, TI_FLAGS, 0);
        hle_OSi_RescheduleThread(c);
    }
}

/* Para depuração: onde cada thread parou. */
void rt_dump_threads(void) {
    for (int i = 0; i < MAX_FIBERS; i++) {
        Fiber *f = &g_fibers[i];
        if (!f->used) continue;
        fprintf(stderr, "  thread %08x%s estado=%u:", f->ctx, f == g_cur ? " (atual)" : "",
                g_cpu_main ? rd32(g_cpu_main, f->ctx + 0x64) : 0);
        if (f != g_cur) for (int k = f->saved.rsp - 1, n = 0; k >= 0 && n < 8; k--, n++) fprintf(stderr, " %08x", f->saved.rs[k]);
        fprintf(stderr, "\n");
    }
}

/* ---------------- tarefas cooperativas do jogo (corrotinas) ----------------
 * Além das threads do SDK, o jogo tem um sistema próprio de tarefas (até 16) que rodam dentro
 * da thread principal, cada uma com a sua pilha. O laço principal chama 0206f014, que retoma
 * a tarefa atual (0206efd8); a tarefa devolve o controle com 0206ee5c (por quadro) e termina em
 * 0206ede4. No DS isso é troca de sp na mão; aqui cada tarefa ganha uma fibra do host.
 *   020b9d4c  sp salvo do laço principal
 *   020b9d50  índice da tarefa atual
 *   020b9d54  sp salvo de cada tarefa (0 = livre)
 *   020b9d94  função de cada tarefa */
#define TASK_MAX 16
#define TASK_CUR 0x020b9d50u
#define TASK_SP  0x020b9d54u
static Fiber *g_task_fiber[TASK_MAX];
static Fiber *g_task_parent;   /* fibra do laço principal enquanto uma tarefa roda */
static int g_task_running = -1;

static Fiber *fiber_alloc_task(void) {
    for (int i = 0; i < MAX_FIBERS; i++) if (g_fibers[i].used == 2) { g_fibers[i].used = 1; return &g_fibers[i]; } /* reaproveita pilha */
    Fiber *f = fiber_new(0);
    f->uc.uc_stack.ss_sp = NULL;
    return f;
}
static void task_entry(void) {
    Cpu *c = g_cpu_main;
    int idx = g_task_running;
    uint32_t sp = rd32(c, TASK_SP + idx * 4);
    /* o quadro inicial montado por 0206ef20 tem 14 palavras; o trampolim 0206eef8 chama a função */
    c->r[13] = sp + 14 * 4;
    c->rsp = 0;
    c->r[14] = 0;
    rt_call(c, 0x0206eef8u); /* termina em hle_Task_Finish, que não volta */
    rt_fatal(c, "tarefa voltou depois de terminar", idx);
}
/* troca da fibra atual para `to`, passando a ela a identidade (ctx) da thread do SDK */
static void task_swap(Cpu *c, Fiber *to, int fresh) {
    Fiber *me = g_cur;
    to->ctx = me->ctx; me->ctx = 0;
    me->saved = *c;
    g_cur = to;
    if (fresh) {
        getcontext(&to->uc);
        if (!to->uc.uc_stack.ss_sp) to->uc.uc_stack.ss_sp = malloc(FIBER_STACK);
        to->uc.uc_stack.ss_size = FIBER_STACK;
        to->uc.uc_link = NULL;
        makecontext(&to->uc, task_entry, 0);
    } else {
        *c = to->saved;
    }
    swapcontext(&me->uc, &to->uc);
    *c = me->saved;
}
static void ensure_main_fiber(Cpu *c) {
    g_cpu_main = c;
    if (!g_cur) g_cur = fiber_new(rd32(c, rd32(c, TI_BASE + 8)));
}
/* 0206efd8: retoma a tarefa atual (chamada pelo laço principal; volta quando ela cede ou termina) */
void hle_Task_Resume(Cpu *c) {
    ensure_main_fiber(c);
    int idx = (int)rd32(c, TASK_CUR);
    if (idx < 0 || idx >= TASK_MAX) rt_fatal(c, "tarefa inválida", idx);
    wr32(c, 0x020b9d4cu, c->r[13]);
    int fresh = !g_task_fiber[idx];
    if (fresh) g_task_fiber[idx] = fiber_alloc_task();
    g_task_parent = g_cur;
    g_task_running = idx;
    task_swap(c, g_task_fiber[idx], fresh);
    g_task_running = -1;
}
/* 0206ee5c: a tarefa cede o resto do quadro */
void hle_Task_Yield(Cpu *c) {
    if (g_task_running < 0 || !g_task_parent) rt_fatal(c, "yield fora de tarefa", c->r[14]);
    uint32_t keep[13];
    memcpy(keep, c->r, sizeof keep); /* a versão original salva r0-r12 e devolve iguais */
    task_swap(c, g_task_parent, 0);
    memcpy(c->r, keep, sizeof keep);
}
/* 0206ede4: a tarefa terminou */
void hle_Task_Finish(Cpu *c) {
    int idx = g_task_running;
    if (idx < 0) rt_fatal(c, "fim de tarefa fora de tarefa", c->r[14]);
    wr32(c, TASK_SP + idx * 4, 0);
    Fiber *me = g_task_fiber[idx];
    g_task_fiber[idx] = NULL;
    me->used = 2; /* a pilha volta a ser usada por outra tarefa */
    task_swap(c, g_task_parent, 0);
    rt_fatal(c, "tarefa terminada foi retomada", idx);
}
