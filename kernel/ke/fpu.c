#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>

/*
 * Lazy FPU. CR0.TS is set on every context switch. The first FX/SSE
 * insn after that takes #NM (vector 7). We FXSAVE the previous owner
 * (if any), FXRSTOR the current thread, clear TS, and go.
 *
 * Why this will fail in production:
 *  - No XSAVE/XSAVEOPT. AVX/AVX-512 state is not covered. Do not
 *    set CR4.OSXSAVE until the save area grows.
 *  - Kernel is built -mno-sse. A kernel #NM is a bug, not a save.
 *    We panic. Enabling XMM in kernel C is a reversal, not a tweak.
 */

#ifdef JASOS_HOST

void fpu_init(void) {}
void fpu_nm(void) {}
void fpu_lazy_switch(void) {}
void fpu_drop(thread_t *t) { (void)t; }

#else

static thread_t *g_fpu_owner;

static void cr0_set_ts(int on)
{
    u64 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    if (on) cr0 |= (1ULL << 3);
    else    cr0 &= ~(1ULL << 3);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}

void fpu_init(void)
{
    u64 cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 1);                 /* MP */
    cr0 &= ~((1ULL << 2) | (1ULL << 3)); /* EM=0 TS=0 for FNINIT */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    __asm__ volatile("fninit");
    u32 mxcsr = 0x1F80u;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));

    cr0_set_ts(1);
    g_fpu_owner = NULL;
    kprintf("fpu: lazy FXSAVE CR0.TS CR4.OSFXSR\n");
}

void fpu_nm(void)
{
    thread_t *t = ke_current();
    if (!t) panic("#NM with no current thread");
    cr0_set_ts(0);
    if (g_fpu_owner && g_fpu_owner != t)
        __asm__ volatile("fxsaveq %0" : "=m"(g_fpu_owner->fpu_state) : : "memory");
    if (t->fpu_used) {
        __asm__ volatile("fxrstorq %0" :: "m"(t->fpu_state) : "memory");
    } else {
        __asm__ volatile("fninit");
        u32 mxcsr = 0x1F80u;
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
        memset(t->fpu_state, 0, 512);
        t->fpu_state[0] = 0x7F;
        t->fpu_state[1] = 0x03; /* FCW 0x037F */
        t->fpu_used = true;
    }
    g_fpu_owner = t;
}

void fpu_lazy_switch(void)
{
    cr0_set_ts(1);
}

void fpu_drop(thread_t *t)
{
    if (!t || g_fpu_owner != t) return;
    u64 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    if (!(cr0 & (1ULL << 3)))
        __asm__ volatile("fxsaveq %0" : "=m"(t->fpu_state) : : "memory");
    g_fpu_owner = NULL;
    cr0_set_ts(1);
}

#endif
