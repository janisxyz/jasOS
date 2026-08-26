#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/fs.h>
#include <jasos/ke.h>

extern int sh_main(int argc, char **argv);

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
    char *av[] = { "sh", NULL };
    sh_main(1, av);
    return 0;
}
