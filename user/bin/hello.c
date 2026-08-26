#include <jasos/syscall.h>
#include <jasos/config.h>
#include <jasos/status.h>

/*
 * Ring-3 hello. Linked as ET_EXEC at 0x400000 against ntdll syscall stubs.
 * Talks to Aegis only through the gate.
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t h = 0;
    status_t st = NtCreateFile(&h, FILE_WRITE_DATA, "/dev/console",
                               FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (NT_SUCCESS(st)) {
        const char msg[] = "hello from ring 3\n";
        u64 n = 0;
        NtWriteFile(h, msg, sizeof(msg) - 1, 0, &n);
        NtClose(h);
    }
    return 0;
}
