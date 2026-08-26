#include <jasos/mm.h>
#include <jasos/ob.h>
#include <jasos/fs.h>
#include <jasos/ke.h>
#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>

#define EXPECT(cond, msg) do { \
    if (!(cond)) { kprintf("FAIL %s\n", msg); return -1; } \
    kprintf("  ok %s\n", msg); \
} while (0)

int selftest_run(void)
{
    kprintf("selftest: begin\n");

    u64 free0 = pmm_free_pages();
    phys_t p = pmm_alloc(3, PMM_KERNEL | PMM_ZERO); /* 8 pages */
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
    /* No current thread: release must fail ownership. */
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

    kprintf("selftest: all assertions passed\n");
    return 0;
}
