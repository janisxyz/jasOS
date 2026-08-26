#include <jasos/syscall.h>
#include <jasos/kprintf.h>

int crash_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("crash: raising STATUS_ACCESS_VIOLATION\n");
    NtRaiseException(STATUS_ACCESS_VIOLATION);
    return 1;
}
