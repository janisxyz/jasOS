#include <jasos/mm.h>
#include <jasos/ob.h>
#include <jasos/fs.h>
#include <jasos/ke.h>
#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>
#include <jasos/elf.h>

#define EXPECT(cond, msg) do { \
    if (!(cond)) { kprintf("FAIL %s\n", msg); return -1; } \
    kprintf("  ok %s\n", msg); \
} while (0)

static volatile int g_ping, g_pong;
static event_object_t *g_e1, *g_e2;

static mutex_object_t *g_pi_m;
static volatile u32 g_pi_prio;
static volatile int g_pi_phase;

static void pi_low(void *arg)
{
    (void)arg;
    ke_acquire_mutex(g_pi_m, (u64)-1);
    g_pi_phase = 1;
    while (g_pi_phase == 1)
        sched_yield();
    g_pi_prio = ke_current()->priority;
    ke_release_mutex(g_pi_m);
}

static void pi_high(void *arg)
{
    (void)arg;
    while (g_pi_phase < 1)
        sched_yield();
    g_pi_phase = 2;
    ke_acquire_mutex(g_pi_m, (u64)-1);
    ke_release_mutex(g_pi_m);
}

static mutex_object_t *g_ab_m;
static volatile status_t g_ab_st;

static void ab_die(void *arg)
{
    (void)arg;
    ke_acquire_mutex(g_ab_m, (u64)-1);
    sched_exit_thread(STATUS_SUCCESS);
}

static void ab_wait(void *arg)
{
    (void)arg;
    g_ab_st = ke_acquire_mutex(g_ab_m, 10000);
    if (g_ab_st == STATUS_ABANDONED || g_ab_st == STATUS_SUCCESS)
        ke_release_mutex(g_ab_m);
}

static event_object_t *g_never;
static volatile int g_victim_ran;
static handle_t g_victim_h;

static void victim_wait(void *arg)
{
    (void)arg;
    g_victim_ran = 1;
    ke_wait_object(&g_never->disp, (u64)-1);
    g_victim_ran = 2;
}

static void killer_fn(void *arg)
{
    (void)arg;
    while (!g_victim_ran)
        sched_yield();
    NtTerminateThread(g_victim_h, STATUS_THREAD_IS_TERMINATING);
}

static void just_exit(void *arg)
{
    (void)arg;
}

static void ping_fn(void *arg)
{
    (void)arg;
    g_ping = 1;
    ke_set_event(g_e1);
    ke_wait_object(&g_e2->disp, (u64)-1);
    g_ping = 2;
}

static void pong_fn(void *arg)
{
    (void)arg;
    ke_wait_object(&g_e1->disp, (u64)-1);
    g_pong = 1;
    ke_set_event(g_e2);
}

int selftest_run(void)
{
    kprintf("selftest: begin\n");

    u64 free0 = pmm_free_pages();
    phys_t p = pmm_alloc(3, PMM_KERNEL | PMM_ZERO);
    EXPECT(p != PMM_INVALID, "pmm_alloc order 3");
    EXPECT(pmm_free_pages() == free0 - 8, "pmm account");
    pmm_free(p, 3);
    EXPECT(pmm_free_pages() == free0, "pmm coalesce");

    void *a = kalloc(64);
    EXPECT(a != NULL, "kalloc 64");
    EXPECT(((uintptr_t)a & 15u) == 0, "kalloc 16-align");
    memset(a, 0x5A, 64);
    kfree(a);
    void *b = kalloc(64);
    EXPECT(b != NULL, "kalloc reuse");
    kfree(b);
    void *big = kalloc(16 * 1024);
    EXPECT(big != NULL, "kalloc large");
    kfree(big);

    ke_pcb()->current_process = psp_system_process();

    status_t st = vfs_mkdir("/tmp/st");
    EXPECT(NT_SUCCESS(st), "vfs mkdir");
    file_object_t *f = NULL;
    st = vfs_open("/tmp/st/file.txt", FILE_READ_DATA | FILE_WRITE_DATA,
                  FILE_CREATE, FILE_NON_DIRECTORY_FILE, &f);
    EXPECT(NT_SUCCESS(st) && f, "vfs create");
    u64 n = 0;
    st = vfs_write(f, "hello-aegis", 11, &n);
    EXPECT(NT_SUCCESS(st) && n == 11, "vfs write");
    f->offset = 0;
    char buf[16];
    memset(buf, 0, sizeof(buf));
    st = vfs_read(f, buf, 11, &n);
    EXPECT(NT_SUCCESS(st) && n == 11 && memcmp(buf, "hello-aegis", 11) == 0, "vfs read");
    ob_dereference(&f->hdr);

    handle_t h = 0;
    st = NtCreateFile(&h, FILE_READ_DATA, "/tmp/st/file.txt", FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    EXPECT(NT_SUCCESS(st) && h != 0, "NtCreateFile");
    memset(buf, 0, sizeof(buf));
    n = 0;
    st = NtReadFile(h, buf, 11, 0, &n);
    EXPECT(NT_SUCCESS(st) && memcmp(buf, "hello-aegis", 11) == 0, "NtReadFile");
    st = NtClose(h);
    EXPECT(NT_SUCCESS(st), "NtClose");
    st = NtClose(h);
    EXPECT(st == STATUS_INVALID_HANDLE, "double close");

    event_object_t *ev = NULL;
    st = ob_create_event("TestEvent", false, false, &ev);
    EXPECT(NT_SUCCESS(st) && ev, "named event");
    object_t *found = NULL;
    st = ob_lookup("\\BaseNamedObjects\\TestEvent", OBJ_EVENT, &found);
    EXPECT(NT_SUCCESS(st) && found == &ev->hdr, "ob_lookup");
    ob_dereference(found);

    handle_t eh = 0;
    st = NtCreateEvent(&eh, NULL, true, false);
    EXPECT(NT_SUCCESS(st), "NtCreateEvent");
    st = NtSetEvent(eh);
    EXPECT(NT_SUCCESS(st), "NtSetEvent");
    NtClose(eh);

    mutex_object_t *mx = NULL;
    st = ob_create_mutex(NULL, false, &mx);
    EXPECT(NT_SUCCESS(st), "mutex create");
    st = ke_release_mutex(mx);
    EXPECT(st == STATUS_MUTANT_NOT_OWNED, "mutex not owned");
    ob_dereference(&mx->hdr);

    sys_mem_info_t mi;
    u64 got = 0;
    st = NtQuerySystemInformation(0, &mi, sizeof(mi), &got);
    EXPECT(NT_SUCCESS(st) && mi.free_pages > 0, "NtQuerySystemInformation mem");

    handle_t dh = 0;
    st = NtCreateFile(&dh, FILE_READ_DATA | DIRECTORY_QUERY, "/etc", FILE_OPEN, FILE_DIRECTORY_FILE);
    EXPECT(NT_SUCCESS(st), "open /etc");
    char dirbuf[256];
    st = NtQueryDirectoryFile(dh, dirbuf, sizeof(dirbuf) - 1, true);
    EXPECT(NT_SUCCESS(st), "readdir /etc");
    kprintf("  /etc: %s\n", dirbuf);
    NtClose(dh);

    handle_t pr = 0, pw = 0;
    st = NtCreatePipe(&pr, &pw);
    EXPECT(NT_SUCCESS(st) && pr && pw, "NtCreatePipe");
    u64 wn = 0, rn = 0;
    st = NtWriteFile(pw, "pipe-ok", 7, 0, &wn);
    EXPECT(NT_SUCCESS(st) && wn == 7, "pipe write");
    char pbuf[16];
    memset(pbuf, 0, sizeof(pbuf));
    st = NtReadFile(pr, pbuf, 7, 0, &rn);
    EXPECT(NT_SUCCESS(st) && rn == 7 && memcmp(pbuf, "pipe-ok", 7) == 0, "pipe read");
    NtClose(pr);
    NtClose(pw);

    /* Dup write end: close original writer, pipe must not EOF until
       the duplicate is closed too. */
    {
        handle_t r = 0, w = 0, w2 = 0;
        st = NtCreatePipe(&r, &w);
        EXPECT(NT_SUCCESS(st), "dup-pipe create");
        st = NtDuplicateObject(HANDLE_CURRENT, w, HANDLE_CURRENT, &w2, FILE_WRITE_DATA, 0);
        EXPECT(NT_SUCCESS(st) && w2, "dup write handle");
        u64 wn = 0, rn = 0;
        st = NtWriteFile(w, "ab", 2, 0, &wn);
        EXPECT(NT_SUCCESS(st) && wn == 2, "dup-pipe write orig");
        NtClose(w);
        char bb[8];
        memset(bb, 0, sizeof(bb));
        st = NtReadFile(r, bb, 2, 0, &rn);
        EXPECT(NT_SUCCESS(st) && rn == 2, "dup-pipe drain after orig close");
        wn = 0;
        st = NtWriteFile(w2, "c", 1, 0, &wn);
        EXPECT(NT_SUCCESS(st) && wn == 1, "dup-pipe write on dup");
        NtClose(w2);
        rn = 0;
        st = NtReadFile(r, bb, 1, 0, &rn);
        EXPECT(NT_SUCCESS(st) && rn == 1 && bb[0] == 'c', "dup-pipe last byte");
        rn = 0;
        st = NtReadFile(r, bb, 1, 0, &rn);
        EXPECT(st == STATUS_END_OF_FILE, "dup-pipe eof after last writer");
        NtClose(r);
    }

    handle_t th = 0;

    st = NtCreateTimer(&th, NULL, true);
    EXPECT(NT_SUCCESS(st) && th, "NtCreateTimer");
    st = NtSetTimer(th, 2, 0);
    EXPECT(NT_SUCCESS(st), "NtSetTimer");
    for (int i = 0; i < 8; i++) ke_on_tick();
    st = NtCancelTimer(th);
    EXPECT(NT_SUCCESS(st), "NtCancelTimer");
    NtClose(th);

    handle_t sec = 0;
    st = NtCreateSection(&sec, SECTION_ALL_ACCESS, 4096, PAGE_READWRITE);
    EXPECT(NT_SUCCESS(st) && sec, "NtCreateSection");
    virt_t mapbase = 0x0000000002000000ULL;
    st = NtMapViewOfSection(sec, HANDLE_CURRENT, &mapbase, 4096, PAGE_READWRITE);
    EXPECT(NT_SUCCESS(st), "NtMapViewOfSection");
    NtUnmapViewOfSection(HANDLE_CURRENT, mapbase);
    NtClose(sec);

    process_t *loader = psp_system_process();
    u8 mini[128];
    u64 elen = elf_make_minimal_hello(mini, sizeof(mini));
    EXPECT(elen == 0x80, "elf_make_minimal_hello");
    virt_t entry = 0;
    st = elf_load(loader, mini, elen, &entry);
    EXPECT(NT_SUCCESS(st) && entry == 0x400070ULL, "elf_load");

    handle_t child = 0;
    st = NtCreateProcess(&child, PROCESS_ALL_ACCESS, "/bin/echo", CREATE_SUSPENDED);
    EXPECT(NT_SUCCESS(st) && child, "NtCreateProcess suspended builtin");
    NtClose(child);

    g_ping = g_pong = 0;
    st = ob_create_event(NULL, false, false, &g_e1);
    EXPECT(NT_SUCCESS(st), "event e1");
    st = ob_create_event(NULL, false, false, &g_e2);
    EXPECT(NT_SUCCESS(st), "event e2");
    thread_t *t1 = NULL, *t2 = NULL;
    st = psp_create_thread(psp_system_process(), "ping", ping_fn, NULL, PRIORITY_NORMAL, 0, &t1);
    EXPECT(NT_SUCCESS(st) && t1, "thread ping");
    st = psp_create_thread(psp_system_process(), "pong", pong_fn, NULL, PRIORITY_NORMAL, 0, &t2);
    EXPECT(NT_SUCCESS(st) && t2, "thread pong");
    kprintf("selftest: entering dispatcher for ping/pong\n");
    sched_start();
    EXPECT(g_ping == 2 && g_pong == 1, "thread ping-pong");

    /* Handle generation: close then reopen the same slot must not
       accept the stale value. */
    {
        handle_t h1 = 0, h2 = 0;
        st = NtCreateFile(&h1, FILE_READ_DATA, "/tmp/st/file.txt", FILE_OPEN, FILE_NON_DIRECTORY_FILE);
        EXPECT(NT_SUCCESS(st) && h1, "handle gen open");
        st = NtClose(h1);
        EXPECT(NT_SUCCESS(st), "handle gen close");
        st = NtClose(h1);
        EXPECT(st == STATUS_INVALID_HANDLE, "stale gen rejected");
        st = NtCreateFile(&h2, FILE_READ_DATA, "/tmp/st/file.txt", FILE_OPEN, FILE_NON_DIRECTORY_FILE);
        EXPECT(NT_SUCCESS(st) && h2 && h2 != h1, "handle gen reuse new value");
        EXPECT(HANDLE_GEN(h2) != 0, "handle gen nonzero");
        NtClose(h2);
    }

    /* /dev/console is a CHAR vnode. */
    {
        handle_t ch = 0;
        st = NtCreateFile(&ch, FILE_WRITE_DATA, "/dev/console", FILE_OPEN, FILE_NON_DIRECTORY_FILE);
        EXPECT(NT_SUCCESS(st) && ch, "/dev/console open");
        u64 wn = 0;
        st = NtWriteFile(ch, "console-ok\n", 11, 0, &wn);
        EXPECT(NT_SUCCESS(st) && wn == 11, "/dev/console write");
        NtClose(ch);
    }

    /* NtQueryVirtualMemory on a mapped section. */
    {
        handle_t sec = 0;
        st = NtCreateSection(&sec, SECTION_ALL_ACCESS, 4096, PAGE_READWRITE);
        EXPECT(NT_SUCCESS(st), "qvm section");
        virt_t mb = 0x0000000002100000ULL;
        st = NtMapViewOfSection(sec, HANDLE_CURRENT, &mb, 4096, PAGE_READWRITE);
        EXPECT(NT_SUCCESS(st), "qvm map");
        memory_basic_information_t inf;
        memset(&inf, 0, sizeof(inf));
        st = NtQueryVirtualMemory(HANDLE_CURRENT, mb, &inf, sizeof(inf));
        EXPECT(NT_SUCCESS(st) && inf.base == mb && inf.region_size >= 4096, "NtQueryVirtualMemory");
        NtUnmapViewOfSection(HANDLE_CURRENT, mb);
        NtClose(sec);
    }

    /* WaitForMultipleObjects WAIT_ANY. */
    {
        handle_t e1 = 0, e2 = 0;
        st = NtCreateEvent(&e1, NULL, true, false);
        EXPECT(NT_SUCCESS(st), "wfmo e1");
        st = NtCreateEvent(&e2, NULL, true, false);
        EXPECT(NT_SUCCESS(st), "wfmo e2");
        handle_t hs[2] = { e1, e2 };
        st = NtWaitForMultipleObjects(hs, 2, false, 0);
        EXPECT(st == STATUS_TIMEOUT, "wfmo timeout");
        st = NtSetEvent(e2);
        EXPECT(NT_SUCCESS(st), "wfmo set e2");
        st = NtWaitForMultipleObjects(hs, 2, false, 0);
        EXPECT(st == (status_t)1, "wfmo wait-any index 1");
        NtClose(e1);
        NtClose(e2);
    }

    {
        handle_t a = 0, b = 0;
        st = NtCreateEvent(&a, NULL, false, false);
        EXPECT(NT_SUCCESS(st), "wfmo-all a");
        st = NtCreateEvent(&b, NULL, false, false);
        EXPECT(NT_SUCCESS(st), "wfmo-all b");
        handle_t pair[2] = { a, b };
        st = NtWaitForMultipleObjects(pair, 2, true, 0);
        EXPECT(st == STATUS_TIMEOUT, "wfmo-all timeout");
        NtSetEvent(a);
        st = NtWaitForMultipleObjects(pair, 2, true, 0);
        EXPECT(st == STATUS_TIMEOUT, "wfmo-all still missing b");
        NtSetEvent(b);
        st = NtWaitForMultipleObjects(pair, 2, true, 0);
        EXPECT(NT_SUCCESS(st), "wfmo-all both signaled");
        NtClose(a);
        NtClose(b);
    }

    /* WFMO bounds: 0 and 17 fail closed. */
    {
        handle_t dummy = 0;
        st = NtWaitForMultipleObjects(&dummy, 0, false, 0);
        EXPECT(st == STATUS_INVALID_PARAMETER, "wfmo count 0");
        handle_t too[17];
        for (int i = 0; i < 17; i++) too[i] = 0;
        st = NtWaitForMultipleObjects(too, 17, false, 0);
        EXPECT(st == STATUS_INVALID_PARAMETER, "wfmo count 17");
    }

    /* W^X PT_LOAD and executable PT_GNU_STACK are refused. */
    {
        u8 wx[128];
        u64 wlen = elf_make_minimal_hello(wx, sizeof(wx));
        EXPECT(wlen == 0x80, "wx hello size");
        wx[68] = 7; /* PT_LOAD PF_R|PF_W|PF_X */
        virt_t ent = 0;
        st = elf_load(psp_system_process(), wx, wlen, &ent);
        EXPECT(st == STATUS_INVALID_IMAGE_FORMAT, "elf W^X PT_LOAD refused");

        u8 gs[192];
        memset(gs, 0, sizeof(gs));
        memcpy(gs, mini, 0x80);
        gs[56] = 2; /* e_phnum = 2 */
        /* second phdr at 64+56=120: PT_GNU_STACK, PF_X */
        gs[120] = 0x51; gs[121] = 0xE5; gs[122] = 0x74; gs[123] = 0x64;
        gs[124] = 1; /* PF_X */
        st = elf_load(psp_system_process(), gs, 192, &ent);
        EXPECT(st == STATUS_INVALID_IMAGE_FORMAT, "elf PT_GNU_STACK+X refused");
    }

    /* Pipe last-writer close yields EOF. */

    {
        handle_t pr = 0, pw = 0;
        st = NtCreatePipe(&pr, &pw);
        EXPECT(NT_SUCCESS(st), "eof pipe create");
        u64 wn = 0, rn = 0;
        st = NtWriteFile(pw, "xy", 2, 0, &wn);
        EXPECT(NT_SUCCESS(st) && wn == 2, "eof pipe write");
        NtClose(pw);
        char pbuf[8];
        memset(pbuf, 0, sizeof(pbuf));
        st = NtReadFile(pr, pbuf, 2, 0, &rn);
        EXPECT(NT_SUCCESS(st) && rn == 2 && pbuf[0] == 'x', "eof pipe drain");
        st = NtReadFile(pr, pbuf, 1, 0, &rn);
        EXPECT(st == STATUS_END_OF_FILE, "eof pipe last writer");
        NtClose(pr);
    }

    /* Ramdisk0 is a real 1 MiB IRP block device. Bytes persist across close. */
    {
        handle_t rh = 0;
        st = NtCreateFile(&rh, FILE_READ_DATA | FILE_WRITE_DATA, "/dev/ram0",
                          FILE_OPEN, FILE_NON_DIRECTORY_FILE);
        EXPECT(NT_SUCCESS(st) && rh, "ram0 open");
        char pat[512];
        memset(pat, 0x5C, sizeof(pat));
        memcpy(pat, "RAMDISK", 7);
        u64 wn = 0, rn = 0;
        st = NtWriteFile(rh, pat, 512, 0, &wn);
        EXPECT(NT_SUCCESS(st) && wn == 512, "ram0 write sector 0");
        memset(pat, 0xA5, sizeof(pat));
        st = NtWriteFile(rh, pat, 512, 4096, &wn);
        EXPECT(NT_SUCCESS(st) && wn == 512, "ram0 write sector 8");
        NtClose(rh);
        rh = 0;
        st = NtCreateFile(&rh, FILE_READ_DATA | FILE_WRITE_DATA, "/dev/ram0",
                          FILE_OPEN, FILE_NON_DIRECTORY_FILE);
        EXPECT(NT_SUCCESS(st) && rh, "ram0 reopen");
        char got[512];
        memset(got, 0, sizeof(got));
        st = NtReadFile(rh, got, 512, 0, &rn);
        EXPECT(NT_SUCCESS(st) && rn == 512 && memcmp(got, "RAMDISK", 7) == 0,
               "ram0 persist sector 0");
        memset(got, 0, sizeof(got));
        st = NtReadFile(rh, got, 512, 4096, &rn);
        EXPECT(NT_SUCCESS(st) && rn == 512 && (u8)got[0] == 0xA5,
               "ram0 persist sector 8");
        char one = 1;
        st = NtWriteFile(rh, &one, 1, RAMDISK_SIZE, &wn);
        EXPECT(st == STATUS_INVALID_PARAMETER, "ram0 oob offset");
        st = NtWriteFile(rh, &one, 1, RAMDISK_SIZE - 1, &wn);
        EXPECT(NT_SUCCESS(st) && wn == 1, "ram0 last byte");
        st = NtWriteFile(rh, pat, 2, RAMDISK_SIZE - 1, &wn);
        EXPECT(st == STATUS_DISK_FULL, "ram0 overflow");
        NtClose(rh);
    }

    /* User-mode NtCreateThread refuses a kernel RIP (SMEP). */
    {
        handle_t ph = 0;
        st = NtCreateProcess(&ph, PROCESS_ALL_ACCESS, "/bin/hello", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st) && ph, "user proc for rip check");
        object_t *o = NULL;
        st = ht_lookup(&psp_system_process()->handles, ph, 0, OBJ_PROCESS, &o);
        EXPECT(NT_SUCCESS(st) && o, "user proc lookup");
        process_t *up = (process_t *)o;
        EXPECT(up->user_mode, "hello is user_mode");
        process_t *saved = ke_current_process();
        ke_pcb()->current_process = up;
        handle_t th = 0;
        st = NtCreateThread(&th, (void (*)(void *))KERNEL_VMA, NULL, 0);
        EXPECT(st == STATUS_ACCESS_VIOLATION, "user thread kernel rip refused");
        ke_pcb()->current_process = saved;

        /* Demand-zero: alloc does not consume PMM; first write populates.
           Guard page is not a VAD so PF/copy must not fill it. */
        {
            u64 free0 = pmm_free_pages();
            virt_t dz = 0x0000000002000000ULL;
            st = vmm_alloc_user(up, &dz, 16u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
            EXPECT(NT_SUCCESS(st) && dz == 0x0000000002000000ULL, "demand-zero alloc");
            EXPECT(pmm_free_pages() == free0, "demand-zero no frames yet");
            int vidx = -1;
            for (u32 i = 0; i < up->aspace.vad_count; i++) {
                if (up->aspace.vads[i].start == dz) { vidx = (int)i; break; }
            }
            EXPECT(vidx >= 0, "demand-zero VAD inserted");
#ifdef JASOS_HOST
            EXPECT(up->aspace.host_pages[vidx] == NULL, "demand-zero shadow lazy");
#endif
            const char msg[] = "demand-zero";
            st = vmm_write_aspace(&up->aspace, dz, msg, sizeof(msg));
            EXPECT(NT_SUCCESS(st), "demand-zero populate write");
#ifdef JASOS_HOST
            EXPECT(up->aspace.host_pages[vidx] && up->aspace.host_pages[vidx][0],
                   "demand-zero page filled");
            EXPECT(up->aspace.host_npages[vidx] == 16, "demand-zero npages");
            EXPECT(up->aspace.host_pages[vidx][1] == NULL, "sibling page still lazy");
#endif
            char back[16];
            memset(back, 0, sizeof(back));
            st = vmm_read_aspace(&up->aspace, back, dz, sizeof(msg));
            EXPECT(NT_SUCCESS(st) && memcmp(back, msg, sizeof(msg)) == 0,
                   "demand-zero readback");
            EXPECT(vmm_handle_user_fault(&up->aspace, dz + PAGE_SIZE, true),
                   "fault demand-zero");
            virt_t overlap = dz + PAGE_SIZE;
            st = vmm_alloc_user(up, &overlap, PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
            EXPECT(st == STATUS_CONFLICTING_ADDRESSES, "demand-zero overlap");
            virt_t hole = USER_STACK_TOP - USER_STACK_SIZE;
            st = vmm_write_aspace(&up->aspace, hole, "x", 1);
            EXPECT(st == STATUS_ACCESS_VIOLATION, "stack guard not demand-zero");
            EXPECT(!vmm_handle_user_fault(&up->aspace, hole, true), "fault miss kills");
            virt_t ro = 0x0000000003000000ULL;
            st = vmm_alloc_user(up, &ro, PAGE_SIZE, PAGE_READONLY, MEM_COMMIT);
            EXPECT(NT_SUCCESS(st), "ro VAD alloc");
            st = vmm_write_aspace(&up->aspace, ro, "x", 1);
            EXPECT(NT_SUCCESS(st), "kernel copy into RO VAD");
            EXPECT(!vmm_handle_user_fault(&up->aspace, ro, true),
                   "user write fault on RO");
            char z = 1;
            st = vmm_read_aspace(&up->aspace, &z, ro, 1);
            EXPECT(NT_SUCCESS(st) && z == 'x', "ro VAD kernel-filled");
            virt_t bomb = 0x0000000004000000ULL;
            st = vmm_alloc_user(up, &bomb, USER_COMMIT_MAX, PAGE_READWRITE, MEM_COMMIT);
            EXPECT(st == STATUS_INSUFFICIENT_RESOURCES, "commit cap");
            st = vmm_free_user(up, dz, 16u * PAGE_SIZE);
            EXPECT(NT_SUCCESS(st), "demand-zero free");
#ifdef JASOS_HOST
            {
                int gone = 1;
                for (u32 i = 0; i < up->aspace.vad_count; i++)
                    if (up->aspace.vads[i].start == dz) gone = 0;
                EXPECT(gone, "demand-zero VAD removed");
            }
#else
            EXPECT(pmm_free_pages() == free0, "demand-zero frames returned");
#endif

            /* Per-page host shadow: a 1 MiB VAD plus one-byte write must
               not allocate a 1 MiB slab. */
            {
                virt_t big = 0x0000000005000000ULL;
                u64 h0 = heap_used();
                st = vmm_alloc_user(up, &big, 256u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
                EXPECT(NT_SUCCESS(st), "1MiB VAD alloc");
                u64 h1 = heap_used();
                char one = 0x5A;
                st = vmm_write_aspace(&up->aspace, big, &one, 1);
                EXPECT(NT_SUCCESS(st), "1MiB VAD first byte");
                u64 h2 = heap_used();
                EXPECT(h2 - h0 < 32u * 1024u, "per-page shadow not whole VAD");
                (void)h1;
                char back = 0;
                st = vmm_read_aspace(&up->aspace, &back, big, 1);
                EXPECT(NT_SUCCESS(st) && back == 0x5A, "1MiB VAD readback");
                st = vmm_free_user(up, big, 256u * PAGE_SIZE);
                EXPECT(NT_SUCCESS(st), "1MiB VAD free");
            }
        }

        ob_dereference(o);
        NtClose(ph);
    }

    /* Wait on a dead thread: signaled on the way to TERMINATED. */
    {
        thread_t *ex = NULL;
        st = psp_create_thread(psp_system_process(), "just-exit", just_exit, NULL,
                               PRIORITY_NORMAL, 0, &ex);
        EXPECT(NT_SUCCESS(st) && ex, "exit-thread create");
        sched_start();
        st = ke_wait_object(&ex->disp, 0);
        EXPECT(st == STATUS_SUCCESS, "wait on dead thread");
        EXPECT(ex->state == THR_TERMINATED, "dead thread TERMINATED");
    }

    /* Wait on a process: /bin/hello ELF stub exits and signals the process. */
    {
        handle_t ph = 0;
        st = NtCreateProcess(&ph, PROCESS_ALL_ACCESS, "/bin/hello", 0);
        EXPECT(NT_SUCCESS(st) && ph, "hello process for wait");
        object_t *o = NULL;
        st = ht_lookup(&psp_system_process()->handles, ph, 0, OBJ_PROCESS, &o);
        EXPECT(NT_SUCCESS(st) && o, "hello process lookup");
        process_t *hp = (process_t *)o;
        sched_start();
        st = ke_wait_object(&hp->disp, 0);
        EXPECT(st == STATUS_SUCCESS, "wait on dead process");
        EXPECT(hp->terminating, "process terminating");
        ob_dereference(o);
        NtClose(ph);
    }

    /* Mutex owner death abandons waiters. */
    {
        g_ab_st = (status_t)0xFFFFFFFFu;
        st = ob_create_mutex(NULL, false, &g_ab_m);
        EXPECT(NT_SUCCESS(st) && g_ab_m, "abandon mutex create");
        thread_t *d = NULL, *w = NULL;
        st = psp_create_thread(psp_system_process(), "ab-die", ab_die, NULL,
                               PRIORITY_HIGH, 0, &d);
        EXPECT(NT_SUCCESS(st) && d, "abandon owner thread");
        st = psp_create_thread(psp_system_process(), "ab-wait", ab_wait, NULL,
                               PRIORITY_NORMAL, 0, &w);
        EXPECT(NT_SUCCESS(st) && w, "abandon waiter thread");
        sched_start();
        EXPECT(g_ab_st == STATUS_ABANDONED, "mutex abandoned on owner death");
    }

    /* Priority inheritance: high waiter donates to low owner. */
    {
        g_pi_phase = 0;
        g_pi_prio = 0;
        st = ob_create_mutex(NULL, false, &g_pi_m);
        EXPECT(NT_SUCCESS(st) && g_pi_m, "pi mutex create");
        thread_t *lo = NULL, *hi = NULL;
        st = psp_create_thread(psp_system_process(), "pi-low", pi_low, NULL,
                               PRIORITY_NORMAL, 0, &lo);
        EXPECT(NT_SUCCESS(st) && lo, "pi low thread");
        st = psp_create_thread(psp_system_process(), "pi-high", pi_high, NULL,
                               PRIORITY_HIGH, 0, &hi);
        EXPECT(NT_SUCCESS(st) && hi, "pi high thread");
        sched_start();
        EXPECT(g_pi_prio == PRIORITY_HIGH, "mutex wait-boost donated");
        EXPECT(lo->priority == PRIORITY_NORMAL, "boost unwound on release");
        EXPECT(lo->wait_boost == 0, "wait_boost cleared");
    }

    /* NtTerminateThread by handle kills a waiter. */
    {
        g_victim_ran = 0;
        g_victim_h = 0;
        st = ob_create_event(NULL, false, false, &g_never);
        EXPECT(NT_SUCCESS(st) && g_never, "kill event");
        thread_t *v = NULL, *k = NULL;
        st = psp_create_thread(psp_system_process(), "victim", victim_wait, NULL,
                               PRIORITY_NORMAL, 0, &v);
        EXPECT(NT_SUCCESS(st) && v, "victim thread");
        st = ht_insert(&psp_system_process()->handles, &v->hdr, THREAD_ALL_ACCESS, &g_victim_h);
        EXPECT(NT_SUCCESS(st) && g_victim_h, "victim handle");
        st = psp_create_thread(psp_system_process(), "killer", killer_fn, NULL,
                               PRIORITY_HIGH, 0, &k);
        EXPECT(NT_SUCCESS(st) && k, "killer thread");
        sched_start();
        EXPECT(v->state == THR_TERMINATED, "victim terminated");
        EXPECT(g_victim_ran == 1, "victim did not resume wait");
        st = ke_wait_object(&v->disp, 0);
        EXPECT(st == STATUS_SUCCESS, "wait on killed thread");
        NtClose(g_victim_h);
    }

    /* Handle inherit: mark a named event, spawn suspended child, child table has it. */
    {
        handle_t eh = 0;
        st = NtCreateEvent(&eh, "InheritMe", false, false);
        EXPECT(NT_SUCCESS(st) && eh, "inherit event");
        st = ht_set_inherit(&psp_system_process()->handles, eh, true);
        EXPECT(NT_SUCCESS(st), "set inherit");
        handle_t ph = 0;
        st = NtCreateProcess(&ph, PROCESS_ALL_ACCESS, "/bin/hello", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st) && ph, "inherit child");
        object_t *o = NULL;
        st = ht_lookup(&psp_system_process()->handles, ph, 0, OBJ_PROCESS, &o);
        EXPECT(NT_SUCCESS(st) && o, "inherit child lookup");
        process_t *ch = (process_t *)o;
        int found = 0;
        for (u32 i = 1; i < HANDLE_TABLE_SLOTS; i++) {
            if (ch->handles.slots[i].object &&
                ch->handles.slots[i].object->type &&
                ch->handles.slots[i].object->type->kind == OBJ_EVENT &&
                ch->handles.slots[i].inherit)
                found = 1;
        }
        EXPECT(found, "child inherited event");
        ob_dereference(o);
        NtClose(ph);
        NtClose(eh);
    }

    /* DuplicateObject DUPLICATE_INHERIT + DUPLICATE_SAME_ACCESS. */
    {
        handle_t e = 0, e2 = 0;
        st = NtCreateEvent(&e, NULL, false, false);
        EXPECT(NT_SUCCESS(st) && e, "dup flags event");
        st = NtDuplicateObject(HANDLE_CURRENT, e, HANDLE_CURRENT, &e2, 0,
                               DUPLICATE_SAME_ACCESS | DUPLICATE_INHERIT);
        EXPECT(NT_SUCCESS(st) && e2 && e2 != e, "dup inherit handle");
        u32 i = HANDLE_INDEX(e2);
        EXPECT(psp_system_process()->handles.slots[i].inherit, "dup inherit bit");
        NtClose(e2);
        NtClose(e);
    }

    kprintf("selftest: all assertions passed\n");
    return 0;
}
