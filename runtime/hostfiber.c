#include <stdlib.h>
#include "hostfiber.h"
#ifdef _WIN32
#include <windows.h>
static void *g_thread_fiber;
static VOID CALLBACK tramp(LPVOID p) { ((HostCtx *)p)->fn(); }
static void ensure_converted(void) { if (!g_thread_fiber) g_thread_fiber = ConvertThreadToFiber(NULL); }
void hostctx_prepare(HostCtx *h, void (*fn)(void), size_t stack) {
    ensure_converted();
    if (h->fib) DeleteFiber(h->fib); /* a fibra antiga já terminou e não é a atual */
    h->fn = fn;
    h->fib = CreateFiber(stack, tramp, h);
}
void hostctx_swap(HostCtx *from, HostCtx *to) {
    ensure_converted();
    if (!from->fib) from->fib = GetCurrentFiber();
    SwitchToFiber(to->fib);
}
#else
void hostctx_prepare(HostCtx *h, void (*fn)(void), size_t stack) {
    getcontext(&h->uc);
    if (!h->stack) h->stack = malloc(stack);
    h->uc.uc_stack.ss_sp = h->stack;
    h->uc.uc_stack.ss_size = stack;
    h->uc.uc_link = NULL;
    makecontext(&h->uc, fn, 0);
}
void hostctx_swap(HostCtx *from, HostCtx *to) { swapcontext(&from->uc, &to->uc); }
#endif
