#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/status.h>
#include <jasos/config.h>

int ls_main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    handle_t h;
    status_t st = NtCreateFile(&h, FILE_READ_DATA | DIRECTORY_QUERY, path,
                               FILE_OPEN, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        kprintf("ls: %s: %s\n", path, status_name(st));
        return 1;
    }
    char buf[1024];
    st = NtQueryDirectoryFile(h, buf, sizeof(buf) - 1, true);
    if (NT_SUCCESS(st)) kprintf("%s\n", buf);
    NtClose(h);
    return NT_SUCCESS(st) ? 0 : 1;
}
