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

    kprintf("selftest: all assertions passed\n");
    return 0;
}
