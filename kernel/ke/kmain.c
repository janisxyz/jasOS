#include <jasos/ke.h>
#include <jasos/mm.h>
#include <jasos/kprintf.h>
#include <jasos/fs.h>
#include <jasos/io.h>
#include <jasos/syscall.h>
#include <jasos/string.h>
#include <jasos/config.h>

#ifdef JASOS_HOST
extern int  host_wants_selftest(void);
extern int  host_wants_shell(void);
extern void host_build_mmap(mmap_entry_t *map, u32 *count);
extern int  selftest_run(void);
#endif

extern int sh_main(int argc, char **argv);
extern int init_main(int argc, char **argv);

static void init_thread(void *arg)
{
    (void)arg;
    kprintf("init: pid %llu\n",
            (unsigned long long)(ke_current_process() ? ke_current_process()->pid : 0));
    init_main(0, NULL);
    /* init does not return; if it does, wait forever. */
    for (;;) NtDelayExecution(10);
}

status_t psp_create_init(void)
{
    process_t *p;
    status_t st = psp_create_process("init", psp_system_process(), &p);
    if (!NT_SUCCESS(st)) return st;
    /* pid should be 1 — first user process after system (pid 0). */
    thread_t *t;
    st = psp_create_thread(p, "init", init_thread, NULL, PRIORITY_NORMAL, 0, &t);
    (void)t;
    kprintf("init: process created pid %llu\n", (unsigned long long)p->pid);
    return st;
}

#ifndef JASOS_HOST
extern u8 _kernel_start[], _kernel_end[];

static u32 mb2_parse(u64 mb2_phys, mmap_entry_t *out, u32 max)
{
    u8 *p = (u8 *)(uintptr_t)mb2_phys;
    u32 total = *(u32 *)p;
    u8 *end = p + total;
    p += 8;
    u32 n = 0;
    while (p + 8 < end) {
        u32 type = *(u32 *)p;
        u32 size = *(u32 *)(p + 4);
        if (type == 0) break;
        if (type == 6 && n < max) { /* mmap */
            u32 esize = *(u32 *)(p + 8);
            u8 *e = p + 16;
            u8 *ee = p + size;
            while (e + esize <= ee && n < max) {
                out[n].base = *(u64 *)e;
                out[n].length = *(u64 *)(e + 8);
                out[n].type = *(u32 *)(e + 16);
                n++;
                e += esize;
            }
        }
        u32 aligned = (size + 7u) & ~7u;
        p += aligned ? aligned : 8;
    }
    return n;
}
#endif

void kmain_early(u64 mb2_phys)
{
    mmap_entry_t map[64];
    u32 nmap = 0;
    phys_t kphys = 0x100000;
    u64 ksize = 0x200000;

    ke_init();
    serial_init();
    kprintf("\n");
    kprintf("jasOS Aegis " JASOS_VERSION_STR " — hybrid kernel\n");
    kprintf("copyright 2026 janisxyz  MIT\n");

#ifdef JASOS_HOST
    (void)mb2_phys;
    host_build_mmap(map, &nmap);
    kprintf("boot: host HAL, fake mmap %u entries\n", nmap);
#else
    nmap = mb2_parse(mb2_phys, map, 64);
    kphys = (phys_t)(uintptr_t)_kernel_start - KERNEL_VMA;
    ksize = (u64)(_kernel_end - _kernel_start);
    kprintf("boot: mb2=%llx kernel phys %llx size %llu\n",
            (unsigned long long)mb2_phys,
            (unsigned long long)kphys,
            (unsigned long long)ksize);
    gdt_init();
    tss_init();
    idt_init();
    pic_remap(0x20, 0x28);
#endif

    pmm_init(map, nmap, kphys, ksize);
#ifndef JASOS_HOST
    vmm_init(kphys, ksize);
#else
    vmm_init(0, 0);
#endif
    heap_init();
    ob_init();
    io_init();
    timer_init();
    sched_init();
    vfs_init();
    vfs_seed_initrd();
#ifndef JASOS_HOST
    pit_init(TIMER_HZ);
    pic_unmask(0);
    syscall_init();
    pci_init();
    kbd_init();
    fpu_init();
    cpu_enable_smap_smep();
    __asm__ volatile("sti");
#endif

#ifdef JASOS_HOST
    if (host_wants_selftest()) {
        int rc = selftest_run();
        if (rc != 0) panic("selftest failed %d", rc);
        kprintf("HOST SELFTEST OK\n");
        return;
    }
#endif

    status_t st = psp_create_init();
    if (!NT_SUCCESS(st)) panic("init create %s", status_name(st));

    kprintf("boot: starting dispatcher\n");
    sched_start();
#ifdef JASOS_HOST
    kprintf("host: dispatcher returned (idle)\n");
#endif
}

#ifndef JASOS_HOST
void kmain(u64 mb2)
{
    kmain_early(mb2);
    panic("kmain returned");
}
#endif
