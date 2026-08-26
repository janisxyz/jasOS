#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/config.h>
#include <jasos/status.h>

int ps_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    sys_process_info_t buf[MAX_PROCESSES];
    u64 got = 0;
    status_t st = NtQuerySystemInformation(5, buf, sizeof(buf), &got);
    if (!NT_SUCCESS(st)) {
        kprintf("ps: %s\n", status_name(st));
        return 1;
    }
    u32 n = (u32)(got / sizeof(buf[0]));
    kprintf("  PID  THR  STATE  IMAGE\n");
    for (u32 i = 0; i < n; i++) {
        kprintf("%5llu  %3u  %s  %s\n",
                (unsigned long long)buf[i].pid, buf[i].threads,
                buf[i].state, buf[i].image);
    }
    return 0;
}
