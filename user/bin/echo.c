#include <jasos/syscall.h>
#include <jasos/config.h>
#include <jasos/status.h>

int printf(const char *fmt, ...);

/*
 * Ring-3 echo. Linked as ET_EXEC at 0x400000 against ntdll + libc.
 * Talks to Aegis only through the gate. argv[0] is the image; operands
 * start at argv[1]. No operands → a single newline (POSIX echo).
 */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) printf(" ");
        printf("%s", argv[i] ? argv[i] : "");
    }
    printf("\n");
    return 0;
}
