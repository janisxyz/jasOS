#include <jasos/syscall.h>
#include <jasos/config.h>
#include <jasos/status.h>

int printf(const char *fmt, ...);

/*
 * Ring-3 ps. ET_EXEC at 0x400000. NtQuerySystemInformation class 5.
 * User printf has no %llu / field width; pids fit in 32-bit v1.
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sys_process_info_t buf[MAX_PROCESSES];
    u64 got = 0;
    status_t st = NtQuerySystemInformation(5, buf, sizeof(buf), &got);
    if (!NT_SUCCESS(st)) {
        printf("ps: %x\n", (unsigned)st);
        return 1;
    }
    u32 n = (u32)(got / sizeof(buf[0]));
    printf("PID THR STATE IMAGE\n");
    for (u32 i = 0; i < n; i++) {
        printf("%d %d %s %s\n", (int)buf[i].pid, (int)buf[i].threads,
               buf[i].state, buf[i].image);
    }
    return 0;
}
