#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/status.h>
#include <jasos/config.h>

int cat_main(int argc, char **argv)
{
    if (argc < 2) { kprintf("cat: usage: cat file\n"); return 1; }
    handle_t h;
    status_t st = NtCreateFile(&h, FILE_READ_DATA, argv[1], FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        kprintf("cat: %s: %s\n", argv[1], status_name(st));
        return 1;
    }
    char buf[256];
    for (;;) {
        u64 n = 0;
        st = NtReadFile(h, buf, sizeof(buf) - 1, (u64)-1, &n);
        if (st == STATUS_END_OF_FILE || n == 0) break;
        if (!NT_SUCCESS(st)) break;
        buf[n] = 0;
        kprintf("%s", buf);
    }
    NtClose(h);
    return 0;
}
