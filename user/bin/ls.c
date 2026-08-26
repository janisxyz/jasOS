#include <jasos/syscall.h>
#include <jasos/config.h>
#include <jasos/status.h>

int printf(const char *fmt, ...);

/*
 * Ring-3 ls. ET_EXEC at 0x400000. Talks to Aegis only through the gate.
 * NtQueryDirectoryFile returns a newline-separated name dump in v1.
 */
int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    handle_t h = 0;
    status_t st = NtCreateFile(&h, FILE_READ_DATA | DIRECTORY_QUERY, path,
                               FILE_OPEN, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        printf("ls: %s: %x\n", path, (unsigned)st);
        return 1;
    }
    char buf[1024];
    buf[0] = 0;
    st = NtQueryDirectoryFile(h, buf, sizeof(buf) - 1, true);
    if (NT_SUCCESS(st) || st == STATUS_END_OF_FILE)
        printf("%s\n", buf);
    NtClose(h);
    return NT_SUCCESS(st) || st == STATUS_END_OF_FILE ? 0 : 1;
}
