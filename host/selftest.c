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
    EXPECT(NT_SUCCESS(st) && child, "NtCreateProcess suspended echo");
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

    /* WAIT_ALL: a mutex the caller already owns counts as signaled.
       Poll (timeout 0) used to return TIMEOUT because signal_state is 0
       while owned. */
    {
        handle_t mh = 0, eh = 0;
        st = NtCreateMutex(&mh, NULL, true);
        EXPECT(NT_SUCCESS(st) && mh, "wfmo-all owned mutex");
        st = NtCreateEvent(&eh, NULL, false, true);
        EXPECT(NT_SUCCESS(st) && eh, "wfmo-all owned event");
        handle_t pair[2] = { mh, eh };
        st = NtWaitForMultipleObjects(pair, 2, true, 0);
        EXPECT(NT_SUCCESS(st), "wfmo-all owned mutex is satisfied");
        NtReleaseMutex(mh);
        NtReleaseMutex(mh); /* recursion from the WAIT_ALL consume */
        NtClose(mh);
        NtClose(eh);
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

    /* NtProtectVirtualMemory: whole-VAD, refuse W^X, NOACCESS kills probe.
       T14: subrange split is real. */
    {
        handle_t ph = 0;
        st = NtCreateProcess(&ph, PROCESS_ALL_ACCESS, "/bin/hello", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st) && ph, "protect proc");
        object_t *o = NULL;
        st = ht_lookup(&psp_system_process()->handles, ph, 0, OBJ_PROCESS, &o);
        EXPECT(NT_SUCCESS(st) && o, "protect proc lookup");
        process_t *pp = (process_t *)o;
        virt_t pv = 0x0000000006000000ULL;
        st = vmm_alloc_user(pp, &pv, 2u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
        EXPECT(NT_SUCCESS(st), "protect alloc");
        char w = 0x11;
        st = vmm_write_aspace(&pp->aspace, pv, &w, 1);
        EXPECT(NT_SUCCESS(st), "protect write before");
        u32 oldp = 0;
        st = NtProtectVirtualMemory(ph, pv, 2u * PAGE_SIZE, PAGE_READONLY, &oldp);
        EXPECT(NT_SUCCESS(st) && oldp == PAGE_READWRITE, "protect to RO");
        EXPECT(!vmm_handle_user_fault(&pp->aspace, pv, true), "protect RO write fault");
        st = NtProtectVirtualMemory(ph, pv, 2u * PAGE_SIZE, PAGE_EXECUTE_READWRITE, &oldp);
        EXPECT(st == STATUS_INVALID_PAGE_PROTECTION, "protect W^X refused");
        st = NtProtectVirtualMemory(ph, pv, 2u * PAGE_SIZE, PAGE_NOACCESS, &oldp);
        EXPECT(NT_SUCCESS(st), "protect NOACCESS");
        EXPECT(!vmm_probe_user(&pp->aspace, pv, 1, false), "NOACCESS probe");
        st = NtProtectVirtualMemory(ph, pv, 2u * PAGE_SIZE, PAGE_READWRITE, &oldp);
        EXPECT(NT_SUCCESS(st) && oldp == PAGE_NOACCESS, "protect restore from NOACCESS");
        EXPECT(vmm_probe_user(&pp->aspace, pv, 1, true), "restored RW probe");
        st = vmm_free_user(pp, pv, 2u * PAGE_SIZE);
        EXPECT(NT_SUCCESS(st), "protect free");

        /* Subrange split: 4 pages, protect the middle 2 to RO. */
        virt_t sp = 0x0000000006100000ULL;
        st = vmm_alloc_user(pp, &sp, 4u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
        EXPECT(NT_SUCCESS(st), "split alloc 4 pages");
        u32 vads0 = pp->aspace.vad_count;
        unsigned char fill[4] = { 0xA0, 0xA1, 0xA2, 0xA3 };
        for (u32 i = 0; i < 4; i++) {
            st = vmm_write_aspace(&pp->aspace, sp + i * PAGE_SIZE, &fill[i], 1);
            EXPECT(NT_SUCCESS(st), i == 0 ? "split write p0" :
                                  i == 1 ? "split write p1" :
                                  i == 2 ? "split write p2" : "split write p3");
        }
        oldp = 0;
        st = NtProtectVirtualMemory(ph, sp + PAGE_SIZE, 2u * PAGE_SIZE, PAGE_READONLY, &oldp);
        EXPECT(NT_SUCCESS(st) && oldp == PAGE_READWRITE, "split protect middle RO");
        EXPECT(pp->aspace.vad_count == vads0 + 2, "split produced 3 VADs");
        memory_basic_information_t mbi;
        st = NtQueryVirtualMemory(ph, sp, &mbi, sizeof(mbi));
        EXPECT(NT_SUCCESS(st) && mbi.prot == PAGE_READWRITE &&
               mbi.region_size == PAGE_SIZE, "split query left RW 1 page");
        st = NtQueryVirtualMemory(ph, sp + PAGE_SIZE, &mbi, sizeof(mbi));
        EXPECT(NT_SUCCESS(st) && mbi.prot == PAGE_READONLY &&
               mbi.region_size == 2u * PAGE_SIZE, "split query mid RO 2 pages");
        st = NtQueryVirtualMemory(ph, sp + 3u * PAGE_SIZE, &mbi, sizeof(mbi));
        EXPECT(NT_SUCCESS(st) && mbi.prot == PAGE_READWRITE &&
               mbi.region_size == PAGE_SIZE, "split query right RW 1 page");
        EXPECT(vmm_handle_user_fault(&pp->aspace, sp, true), "split left still writable");
        EXPECT(!vmm_handle_user_fault(&pp->aspace, sp + PAGE_SIZE, true),
               "split mid write fault");
        EXPECT(!vmm_handle_user_fault(&pp->aspace, sp + 2u * PAGE_SIZE, true),
               "split mid2 write fault");
        EXPECT(vmm_handle_user_fault(&pp->aspace, sp + 3u * PAGE_SIZE, true),
               "split right still writable");
        unsigned char back = 0;
        st = vmm_read_aspace(&pp->aspace, &back, sp + PAGE_SIZE, 1);
        EXPECT(NT_SUCCESS(st) && back == 0xA1, "split mid data survived");

        /* Restore middle to RW; coalesce must glue the three RW VADs
           so the original 4-page range is nameable again. */
        {
            virt_t cq = 0x0000000006300000ULL;
            st = vmm_alloc_user(pp, &cq, 4u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
            EXPECT(NT_SUCCESS(st), "coalesce alloc");
            u32 oldc = 0;
            st = NtProtectVirtualMemory(ph, cq + PAGE_SIZE, 2u * PAGE_SIZE, PAGE_READONLY, &oldc);
            EXPECT(NT_SUCCESS(st), "coalesce split middle");
            EXPECT(pp->aspace.vad_count >= 3, "coalesce has pieces");
            st = NtProtectVirtualMemory(ph, cq + PAGE_SIZE, 2u * PAGE_SIZE, PAGE_READWRITE, &oldc);
            EXPECT(NT_SUCCESS(st) && oldc == PAGE_READONLY, "coalesce restore middle RW");
            memory_basic_information_t mb2;
            st = NtQueryVirtualMemory(ph, cq, &mb2, sizeof(mb2));
            EXPECT(NT_SUCCESS(st) && mb2.region_size == 4u * PAGE_SIZE &&
                   mb2.prot == PAGE_READWRITE, "coalesce glued 4 pages");
            st = NtProtectVirtualMemory(ph, cq, 4u * PAGE_SIZE, PAGE_READONLY, &oldc);
            EXPECT(NT_SUCCESS(st) && oldc == PAGE_READWRITE, "coalesce whole range nameable");
            st = vmm_free_user(pp, cq, 0);
            EXPECT(NT_SUCCESS(st), "coalesce free");
        }

        /* Prefix protect of the right 1-page VAD. */
        st = NtProtectVirtualMemory(ph, sp + 3u * PAGE_SIZE, PAGE_SIZE, PAGE_READONLY, &oldp);
        EXPECT(NT_SUCCESS(st) && oldp == PAGE_READWRITE, "split prefix/exact right to RO");

        /* Free the middle 2 pages; left and right remain. */
        u64 commit0 = pp->aspace.committed_pages;
        st = vmm_free_user(pp, sp + PAGE_SIZE, 2u * PAGE_SIZE);
        EXPECT(NT_SUCCESS(st), "split free middle");
        EXPECT(pp->aspace.committed_pages == commit0 - 2, "split free dropped commit");
        st = vmm_read_aspace(&pp->aspace, &back, sp, 1);
        EXPECT(NT_SUCCESS(st) && back == 0xA0, "split left survived free");
        st = vmm_read_aspace(&pp->aspace, &back, sp + PAGE_SIZE, 1);
        EXPECT(st == STATUS_ACCESS_VIOLATION, "split hole is gone");
        st = vmm_read_aspace(&pp->aspace, &back, sp + 3u * PAGE_SIZE, 1);
        EXPECT(NT_SUCCESS(st) && back == 0xA3, "split right survived free");

        /* Hole can be reallocated. */
        virt_t hole = sp + PAGE_SIZE;
        st = vmm_alloc_user(pp, &hole, 2u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
        EXPECT(NT_SUCCESS(st) && hole == sp + PAGE_SIZE, "split hole realloc");

        /* size 0 frees the whole VAD at base. */
        st = vmm_free_user(pp, sp, 0);
        EXPECT(NT_SUCCESS(st), "split free size 0 left");
        st = vmm_read_aspace(&pp->aspace, &back, sp, 1);
        EXPECT(st == STATUS_ACCESS_VIOLATION, "size 0 freed the left VAD");
        st = vmm_read_aspace(&pp->aspace, &back, sp + PAGE_SIZE, 1);
        EXPECT(NT_SUCCESS(st), "size 0 did not eat the hole realloc");

        /* Range not in any VAD. */
        st = vmm_free_user(pp, 0x0000000007000000ULL, PAGE_SIZE);
        EXPECT(st == STATUS_CONFLICTING_ADDRESSES, "free miss is CONFLICTING");
        st = NtProtectVirtualMemory(ph, 0x0000000007000000ULL, PAGE_SIZE, PAGE_READONLY, &oldp);
        EXPECT(st == STATUS_CONFLICTING_ADDRESSES, "protect miss is CONFLICTING");

        /* Prefix free of a 3-page VAD. */
        virt_t pf = 0x0000000006200000ULL;
        st = vmm_alloc_user(pp, &pf, 3u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
        EXPECT(NT_SUCCESS(st), "prefix-free alloc");
        unsigned char c0 = 0xB0, c1 = 0xB1, c2 = 0xB2;
        vmm_write_aspace(&pp->aspace, pf, &c0, 1);
        vmm_write_aspace(&pp->aspace, pf + PAGE_SIZE, &c1, 1);
        vmm_write_aspace(&pp->aspace, pf + 2u * PAGE_SIZE, &c2, 1);
        st = vmm_free_user(pp, pf, PAGE_SIZE);
        EXPECT(NT_SUCCESS(st), "prefix free first page");
        st = vmm_read_aspace(&pp->aspace, &back, pf, 1);
        EXPECT(st == STATUS_ACCESS_VIOLATION, "prefix free dropped page 0");
        st = vmm_read_aspace(&pp->aspace, &back, pf + PAGE_SIZE, 1);
        EXPECT(NT_SUCCESS(st) && back == 0xB1, "prefix free kept page 1");
        st = vmm_read_aspace(&pp->aspace, &back, pf + 2u * PAGE_SIZE, 1);
        EXPECT(NT_SUCCESS(st) && back == 0xB2, "prefix free kept page 2");
        st = NtQueryVirtualMemory(ph, pf + PAGE_SIZE, &mbi, sizeof(mbi));
        EXPECT(NT_SUCCESS(st) && mbi.base == pf + PAGE_SIZE &&
               mbi.region_size == 2u * PAGE_SIZE, "prefix free shrunk VAD");

        /* Suffix free. */
        st = vmm_free_user(pp, pf + 2u * PAGE_SIZE, PAGE_SIZE);
        EXPECT(NT_SUCCESS(st), "suffix free last page");
        st = vmm_read_aspace(&pp->aspace, &back, pf + PAGE_SIZE, 1);
        EXPECT(NT_SUCCESS(st) && back == 0xB1, "suffix free kept page 1");
        st = vmm_read_aspace(&pp->aspace, &back, pf + 2u * PAGE_SIZE, 1);
        EXPECT(st == STATUS_ACCESS_VIOLATION, "suffix free dropped page 2");

        st = vmm_free_user(pp, pf + PAGE_SIZE, 0);
        EXPECT(NT_SUCCESS(st), "size 0 remaining page");
        st = vmm_free_user(pp, hole, 0);
        EXPECT(NT_SUCCESS(st), "size 0 hole realloc");

        ob_dereference(o);
        NtClose(ph);
    }

    /* T15: /bin/echo is an ET_EXEC, not a kernel builtin. */
    {
        EXPECT(!builtin_lookup("/bin/echo", NULL), "echo not a kernel builtin");
        EXPECT(builtin_lookup("/bin/sh", NULL), "sh still a kernel builtin");
        u8 *bytes = NULL;
        u64 len = 0;
        st = vfs_read_all("/bin/echo", &bytes, &len);
        EXPECT(NT_SUCCESS(st) && bytes && len > 64, "echo vfs blob");
        EXPECT(bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
               bytes[3] == 'F', "echo is ELF");
        kfree(bytes);
        handle_t eh = 0;
        st = NtCreateProcess(&eh, PROCESS_ALL_ACCESS, "/bin/echo", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st) && eh, "echo process");
        object_t *eo = NULL;
        st = ht_lookup(&psp_system_process()->handles, eh, 0, OBJ_PROCESS, &eo);
        EXPECT(NT_SUCCESS(st) && eo, "echo process lookup");
        process_t *ep = (process_t *)eo;
        EXPECT(ep->user_mode, "echo is user_mode");
        EXPECT(ep->aspace.vad_count >= 1, "echo has image VAD");
        EXPECT(ep->argc == 1, "echo default argc 1");
        EXPECT(strcmp(ep->argv[0], "/bin/echo") == 0, "echo default argv0");
        ob_dereference(eo);
        NtClose(eh);
    }

    /* T15: free of a populated 32-page VAD returns the VA hole. */
    {
        handle_t ph = 0;
        st = NtCreateProcess(&ph, PROCESS_ALL_ACCESS, "/bin/hello", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st), "t15 proc");
        object_t *o = NULL;
        st = ht_lookup(&psp_system_process()->handles, ph, 0, OBJ_PROCESS, &o);
        EXPECT(NT_SUCCESS(st) && o, "t15 proc lookup");
        process_t *pp = (process_t *)o;
        virt_t big = 0x0000000006000000ULL;
        st = vmm_alloc_user(pp, &big, 32u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
        EXPECT(NT_SUCCESS(st), "t15 32-page alloc");
        u32 filled = 0;
        for (u32 i = 0; i < 32; i++) {
            char c = (char)i;
            if (NT_SUCCESS(vmm_write_aspace(&pp->aspace, big + (u64)i * PAGE_SIZE, &c, 1)))
                filled++;
        }
        EXPECT(filled == 32, "t15 populate 32 pages");
        st = vmm_free_user(pp, big, 32u * PAGE_SIZE);
        EXPECT(NT_SUCCESS(st), "t15 32-page free");
        int gone = 1;
        for (u32 i = 0; i < pp->aspace.vad_count; i++)
            if (pp->aspace.vads[i].start == big) gone = 0;
        EXPECT(gone, "t15 VAD gone after free");
        st = vmm_alloc_user(pp, &big, 32u * PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
        EXPECT(NT_SUCCESS(st) && big == 0x0000000006000000ULL, "t15 hole realloc");
        st = vmm_free_user(pp, big, 0);
        EXPECT(NT_SUCCESS(st), "t15 size-0 release");
        ob_dereference(o);
        NtClose(ph);
    }

    /* T16: NtCreateProcessEx copies argv onto the user stack. */
    {
        const char *av[] = { "/bin/echo", "hello", "from", "argv" };
        st = NtCreateProcessEx(NULL, PROCESS_ALL_ACCESS, "/bin/echo", CREATE_SUSPENDED,
                               av, 4);
        EXPECT(st == STATUS_INVALID_PARAMETER, "ex null out");
        handle_t bad = 0;
        st = NtCreateProcessEx(&bad, PROCESS_ALL_ACCESS, "/bin/echo", CREATE_SUSPENDED,
                               av, USER_ARGC_MAX + 1);
        EXPECT(st == STATUS_INVALID_PARAMETER, "ex argc cap");
        st = NtCreateProcessEx(&bad, PROCESS_ALL_ACCESS, "/bin/echo", CREATE_SUSPENDED,
                               NULL, 3);
        EXPECT(st == STATUS_INVALID_PARAMETER, "ex argc without argv");

        handle_t eh = 0;
        st = NtCreateProcessEx(&eh, PROCESS_ALL_ACCESS, "/bin/echo", CREATE_SUSPENDED,
                               av, 4);
        EXPECT(NT_SUCCESS(st) && eh, "ex echo process");
        object_t *eo = NULL;
        st = ht_lookup(&psp_system_process()->handles, eh, 0, OBJ_PROCESS, &eo);
        EXPECT(NT_SUCCESS(st) && eo, "ex echo lookup");
        process_t *ep = (process_t *)eo;
        EXPECT(ep->argc == 4, "ex argc 4");
        EXPECT(strcmp(ep->argv[0], "/bin/echo") == 0, "ex argv0");
        EXPECT(strcmp(ep->argv[1], "hello") == 0, "ex argv1");
        EXPECT(strcmp(ep->argv[2], "from") == 0, "ex argv2");
        EXPECT(strcmp(ep->argv[3], "argv") == 0, "ex argv3");
        EXPECT(ep->user_stack != 0, "ex stack written");

        u64 argc64 = 0;
        st = vmm_read_aspace(&ep->aspace, &argc64, ep->user_stack, 8);
        EXPECT(NT_SUCCESS(st) && argc64 == 4, "ex stack argc");
        u64 ap[5];
        st = vmm_read_aspace(&ep->aspace, ap, ep->user_stack + 8, sizeof(ap));
        EXPECT(NT_SUCCESS(st), "ex stack argv vector");
        EXPECT(ap[4] == 0, "ex argv NULL terminator");
        char s0[16], s1[16], s2[16], s3[16];
        memset(s0, 0, sizeof(s0));
        memset(s1, 0, sizeof(s1));
        memset(s2, 0, sizeof(s2));
        memset(s3, 0, sizeof(s3));
        EXPECT(NT_SUCCESS(vmm_read_aspace(&ep->aspace, s0, ap[0], 11)) &&
               strcmp(s0, "/bin/echo") == 0, "ex stack argv0 string");
        EXPECT(NT_SUCCESS(vmm_read_aspace(&ep->aspace, s1, ap[1], 6)) &&
               strcmp(s1, "hello") == 0, "ex stack argv1 string");
        EXPECT(NT_SUCCESS(vmm_read_aspace(&ep->aspace, s2, ap[2], 5)) &&
               strcmp(s2, "from") == 0, "ex stack argv2 string");
        EXPECT(NT_SUCCESS(vmm_read_aspace(&ep->aspace, s3, ap[3], 5)) &&
               strcmp(s3, "argv") == 0, "ex stack argv3 string");

        ob_dereference(eo);
        NtClose(eh);
    }

    /* T17: Token is an object. Open requires PROCESS_QUERY_INFORMATION.
       TOKEN_QUERY is required to read it. TOKEN_DUPLICATE copies the
       snapshot so a closed process handle does not kill the token. */
    {
        handle_t ph = 0;
        st = NtCreateProcess(&ph, PROCESS_ALL_ACCESS, "/bin/hello", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st), "token proc");
        object_t *o = NULL;
        st = ht_lookup(&psp_system_process()->handles, ph, 0, OBJ_PROCESS, &o);
        EXPECT(NT_SUCCESS(st) && o, "token proc lookup");
        process_t *pp = (process_t *)o;
        EXPECT(pp->token != NULL, "process has token");
        EXPECT(pp->token->hdr.type && pp->token->hdr.type->kind == OBJ_TOKEN,
               "token kind");
        EXPECT(pp->token->pid == pp->pid, "token pid");
        EXPECT(pp->token->integrity == 1, "token admin until logon");
        kpid_t tok_pid = pp->pid;
        ob_dereference(o);

        handle_t th = 0;
        st = NtOpenProcessToken(ph, 0, &th);
        EXPECT(st == STATUS_INVALID_PARAMETER, "open token access 0");
        st = NtOpenProcessToken(ph, TOKEN_QUERY, NULL);
        EXPECT(st == STATUS_INVALID_PARAMETER, "open token null out");
        st = NtOpenProcessToken(ph, TOKEN_QUERY, &th);
        EXPECT(NT_SUCCESS(st) && th, "open token");

        token_basic_information_t inf;
        memset(&inf, 0, sizeof(inf));
        st = NtQueryInformationToken(th, &inf, sizeof(inf));
        EXPECT(NT_SUCCESS(st), "query token");
        EXPECT(inf.pid == tok_pid, "query token pid");
        EXPECT(inf.integrity == 1, "query token admin until logon");
        st = NtQueryInformationToken(th, &inf, 1);
        EXPECT(st == STATUS_INFO_LENGTH_MISMATCH, "query token short buf");

        object_basic_information_t obi;
        memset(&obi, 0, sizeof(obi));
        st = NtQueryObject(th, &obi, sizeof(obi));
        EXPECT(NT_SUCCESS(st) && obi.kind == OBJ_TOKEN, "query object token kind");

        handle_t th_dup_only = 0;
        st = NtOpenProcessToken(ph, TOKEN_DUPLICATE, &th_dup_only);
        EXPECT(NT_SUCCESS(st) && th_dup_only, "open token dup only");
        st = NtQueryInformationToken(th_dup_only, &inf, sizeof(inf));
        EXPECT(st == STATUS_ACCESS_DENIED, "dup-only cannot query");

        handle_t th_copy = 0;
        st = NtDuplicateToken(th, TOKEN_QUERY, &th_copy);
        EXPECT(st == STATUS_ACCESS_DENIED, "query-only cannot duplicate");
        st = NtDuplicateToken(th_dup_only, TOKEN_QUERY, &th_copy);
        EXPECT(NT_SUCCESS(st) && th_copy, "duplicate token");
        memset(&inf, 0, sizeof(inf));
        st = NtQueryInformationToken(th_copy, &inf, sizeof(inf));
        EXPECT(NT_SUCCESS(st) && inf.pid == tok_pid && inf.integrity == 1,
               "duplicate token query");

        handle_t ph_term = 0;
        st = NtCreateProcess(&ph_term, PROCESS_TERMINATE, "/bin/hello", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st), "term-only proc");
        handle_t th_denied = 0;
        st = NtOpenProcessToken(ph_term, TOKEN_QUERY, &th_denied);
        EXPECT(st == STATUS_ACCESS_DENIED, "terminate-only cannot open token");

        handle_t selftok = 0;
        st = NtOpenProcessToken(HANDLE_CURRENT, TOKEN_QUERY, &selftok);
        EXPECT(NT_SUCCESS(st) && selftok, "open current token");
        memset(&inf, 0, sizeof(inf));
        st = NtQueryInformationToken(selftok, &inf, sizeof(inf));
        EXPECT(NT_SUCCESS(st) && inf.pid == 0 && inf.integrity == 1,
               "system token pid 0 admin");

        st = NtWaitForSingleObject(th, 0);
        EXPECT(st == STATUS_INVALID_PARAMETER || st == STATUS_ACCESS_DENIED,
               "token is not waitable");

        /* T18 start: drop-only integrity. Cannot raise. */
        handle_t th_adj = 0;
        st = NtOpenProcessToken(ph, TOKEN_ADJUST | TOKEN_QUERY, &th_adj);
        EXPECT(NT_SUCCESS(st) && th_adj, "open token adjust");
        st = NtSetInformationToken(th, 0);
        EXPECT(st == STATUS_ACCESS_DENIED, "query-only cannot adjust");
        st = NtSetInformationToken(th_adj, 2);
        EXPECT(st == STATUS_INVALID_PARAMETER, "integrity > 1 refused");
        st = NtSetInformationToken(th_adj, 0);
        EXPECT(NT_SUCCESS(st), "drop integrity to user");
        memset(&inf, 0, sizeof(inf));
        st = NtQueryInformationToken(th_adj, &inf, sizeof(inf));
        EXPECT(NT_SUCCESS(st) && inf.integrity == 0, "query dropped integrity");
        st = NtSetInformationToken(th_adj, 1);
        EXPECT(st == STATUS_ACCESS_DENIED, "cannot raise integrity");
        memset(&inf, 0, sizeof(inf));
        st = NtQueryInformationToken(th, &inf, sizeof(inf));
        EXPECT(NT_SUCCESS(st) && inf.integrity == 0, "same token object dropped");
        memset(&inf, 0, sizeof(inf));
        st = NtQueryInformationToken(th_copy, &inf, sizeof(inf));
        EXPECT(NT_SUCCESS(st) && inf.integrity == 1, "duplicate token independent");

        /* T19: spawn from a dropped parent cannot raise. */
        object_t *po2 = NULL;
        st = ht_lookup(&psp_system_process()->handles, ph, 0, OBJ_PROCESS, &po2);
        EXPECT(NT_SUCCESS(st) && po2, "dropped proc lookup");
        process_t *dropped = (process_t *)po2;
        EXPECT(dropped->token && dropped->token->integrity == 0,
               "parent token still dropped");
        process_t *child = NULL;
        st = psp_create_process("/bin/hello", dropped, &child);
        EXPECT(NT_SUCCESS(st) && child && child->token, "inherit spawn");
        EXPECT(child->token->integrity == 0, "child inherits dropped integrity");
        EXPECT(child->token->pid == child->pid, "inherited token pid is child");
        ob_dereference(&child->hdr);
        ob_dereference(po2);

        process_t *admin_child = NULL;
        st = psp_create_process("/bin/hello", psp_system_process(), &admin_child);
        EXPECT(NT_SUCCESS(st) && admin_child && admin_child->token, "system spawn");
        EXPECT(admin_child->token->integrity == 1, "system child still admin");
        ob_dereference(&admin_child->hdr);

        NtClose(th);
        NtClose(th_dup_only);
        NtClose(th_copy);
        NtClose(th_adj);
        NtClose(selftok);
        NtClose(ph);
        NtClose(ph_term);
    }

    /* T19: /bin/ls and /bin/cat are ET_EXEC, not kernel builtins. */
    {
        EXPECT(!builtin_lookup("/bin/ls", NULL), "ls not a kernel builtin");
        EXPECT(!builtin_lookup("/bin/cat", NULL), "cat not a kernel builtin");
        EXPECT(builtin_lookup("/bin/sh", NULL), "sh still a kernel builtin");
        u8 *bytes = NULL;
        u64 len = 0;
        st = vfs_read_all("/bin/ls", &bytes, &len);
        EXPECT(NT_SUCCESS(st) && bytes && len > 64, "ls vfs blob");
        EXPECT(bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
               bytes[3] == 'F', "ls is ELF");
        kfree(bytes);
        bytes = NULL; len = 0;
        st = vfs_read_all("/bin/cat", &bytes, &len);
        EXPECT(NT_SUCCESS(st) && bytes && len > 64, "cat vfs blob");
        EXPECT(bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
               bytes[3] == 'F', "cat is ELF");
        kfree(bytes);

        handle_t lh = 0;
        const char *lav[] = { "/bin/ls", "/etc" };
        st = NtCreateProcessEx(&lh, PROCESS_ALL_ACCESS, "/bin/ls", CREATE_SUSPENDED,
                               lav, 2);
        EXPECT(NT_SUCCESS(st) && lh, "ls process");
        object_t *lo = NULL;
        st = ht_lookup(&psp_system_process()->handles, lh, 0, OBJ_PROCESS, &lo);
        EXPECT(NT_SUCCESS(st) && lo, "ls process lookup");
        process_t *lp = (process_t *)lo;
        EXPECT(lp->user_mode, "ls is user_mode");
        EXPECT(lp->argc == 2, "ls argc 2");
        EXPECT(strcmp(lp->argv[1], "/etc") == 0, "ls argv1 path");
        EXPECT(lp->token && lp->token->integrity == 1, "ls from system is admin");
        ob_dereference(lo);
        NtClose(lh);

        handle_t ch = 0;
        const char *cav[] = { "/bin/cat", "/etc/hostname" };
        st = NtCreateProcessEx(&ch, PROCESS_ALL_ACCESS, "/bin/cat", CREATE_SUSPENDED,
                               cav, 2);
        EXPECT(NT_SUCCESS(st) && ch, "cat process");
        object_t *co = NULL;
        st = ht_lookup(&psp_system_process()->handles, ch, 0, OBJ_PROCESS, &co);
        EXPECT(NT_SUCCESS(st) && co, "cat process lookup");
        process_t *cp = (process_t *)co;
        EXPECT(cp->user_mode, "cat is user_mode");
        EXPECT(cp->argc == 2 && strcmp(cp->argv[1], "/etc/hostname") == 0,
               "cat argv1 file");
        ob_dereference(co);
        NtClose(ch);

        EXPECT(!builtin_lookup("/bin/ps", NULL), "ps not a kernel builtin");
        bytes = NULL; len = 0;
        st = vfs_read_all("/bin/ps", &bytes, &len);
        EXPECT(NT_SUCCESS(st) && bytes && len > 64, "ps vfs blob");
        EXPECT(bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
               bytes[3] == 'F', "ps is ELF");
        kfree(bytes);
        handle_t psh = 0;
        st = NtCreateProcess(&psh, PROCESS_ALL_ACCESS, "/bin/ps", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st) && psh, "ps process");
        object_t *pso = NULL;
        st = ht_lookup(&psp_system_process()->handles, psh, 0, OBJ_PROCESS, &pso);
        EXPECT(NT_SUCCESS(st) && pso, "ps process lookup");
        process_t *psp = (process_t *)pso;
        EXPECT(psp->user_mode, "ps is user_mode");
        ob_dereference(pso);
        NtClose(psh);

        EXPECT(!builtin_lookup("/bin/crash", NULL), "crash not a kernel builtin");
        bytes = NULL; len = 0;
        st = vfs_read_all("/bin/crash", &bytes, &len);
        EXPECT(NT_SUCCESS(st) && bytes && len > 64, "crash vfs blob");
        EXPECT(bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
               bytes[3] == 'F', "crash is ELF");
        kfree(bytes);
        handle_t crh = 0;
        st = NtCreateProcess(&crh, PROCESS_ALL_ACCESS, "/bin/crash", CREATE_SUSPENDED);
        EXPECT(NT_SUCCESS(st) && crh, "crash process");
        object_t *cro = NULL;
        st = ht_lookup(&psp_system_process()->handles, crh, 0, OBJ_PROCESS, &cro);
        EXPECT(NT_SUCCESS(st) && cro, "crash process lookup");
        EXPECT(((process_t *)cro)->user_mode, "crash is user_mode");
        ob_dereference(cro);
        NtClose(crh);
    }

    kprintf("selftest: all assertions passed\n");
    return 0;
}
