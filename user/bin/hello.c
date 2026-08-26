#include <jasos/syscall.h>
#include <jasos/config.h>
#include <jasos/status.h>

int printf(const char *fmt, ...);

/*
 * Ring-3 hello. Linked as ET_EXEC at 0x400000 against ntdll syscall stubs
 * and a tiny libc. Talks to Aegis only through the gate.
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("hello from ring 3\n");
    return 0;
}