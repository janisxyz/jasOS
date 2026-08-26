#include <jasos/syscall.h>
#include <jasos/config.h>
#include <jasos/status.h>

int printf(const char *fmt, ...);

/*
 * Ring-3 cat. ET_EXEC at 0x400000. NUL in the file truncates a chunk
 * the same way the old kernel builtin did — not a binary cat.
 */
int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("cat: usage: cat file\n");
        return 1;
    }
    handle_t h = 0;
    status_t st = NtCreateFile(&h, FILE_READ_DATA, argv[1], FILE_OPEN,
                               FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        printf("cat: %s: %x\n", argv[1], (unsigned)st);
        return 1;
    }
    char buf[256];
    for (;;) {
        u64 n = 0;
        st = NtReadFile(h, buf, sizeof(buf) - 1, (u64)-1, &n);
        if (st == STATUS_END_OF_FILE || n == 0) break;
        if (!NT_SUCCESS(st)) break;
        buf[n] = 0;
        printf("%s", buf);
    }
    NtClose(h);
    return 0;
}
