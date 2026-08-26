#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>
#include <jasos/ke.h>

#ifdef JASOS_HOST
extern int host_wants_shell(void);
#endif

static void cat_path(const char *path)
{
    handle_t h;
    status_t st = NtCreateFile(&h, FILE_READ_DATA, path, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) return;
    char buf[256];
    u64 n = 0;
    st = NtReadFile(h, buf, sizeof(buf) - 1, 0, &n);
    if (NT_SUCCESS(st) || st == STATUS_END_OF_FILE) {
        buf[n] = 0;
        kprintf("%s", buf);
    }
    NtClose(h);
}

int init_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cat_path("/etc/motd");
#ifdef JASOS_HOST
    if (!host_wants_shell()) {
        kprintf("init: host non-interactive, idling\n");
        return 0;
    }
#endif
    kprintf("init: launching sh\n");
    handle_t sh = 0;
    const char *av[] = { "/bin/sh" };
    const char *ev[] = { "PATH=/bin:/usr/bin", "HOME=/" };
    process_create_info_t inf;
    memset(&inf, 0, sizeof(inf));
    inf.argc = 1;
    inf.argv = av;
    inf.envc = 2;
    inf.envp = ev;
    status_t st = NtCreateProcess2(&sh, PROCESS_ALL_ACCESS, "/bin/sh", 0, &inf);
    if (!NT_SUCCESS(st)) {
        kprintf("init: spawn sh failed %s\n", status_name(st));
        return 0;
    }
    NtWaitForSingleObject(sh, (u64)-1);
    NtClose(sh);
    kprintf("init: sh exited, idling\n");
    return 0;
}
