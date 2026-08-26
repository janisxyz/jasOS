#include <jasos/syscall.h>
#include <jasos/status.h>

int printf(const char *fmt, ...);

/*
 * Ring-3 crash. ET_EXEC. NtRaiseException kills the thread, not the kernel.
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("crash: raising ACCESS_VIOLATION\n");
    NtRaiseException(STATUS_ACCESS_VIOLATION);
    return 1;
}
