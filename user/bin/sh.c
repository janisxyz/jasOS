#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/fs.h>
#include <jasos/config.h>
#include <jasos/status.h>
#include <jasos/ke.h>

/*
 * Shell. Uses Nt* — the same surface a user ELF will hit through syscall.
 */

static int split(char *line, char **av, int max)
{
    int n = 0;
    while (*line && n + 1 < max) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;
        av[n++] = line;
        while (*line && *line != ' ' && *line != '\t') line++;
        if (*line) *line++ = 0;
    }
    av[n] = NULL;
    return n;
}

static void cmd_help(void)
{
    kprintf("jasOS sh — commands talk to Aegis through Nt*\n");
    kprintf("  help ps ls cat echo mkdir touch rm cd pwd\n");
    kprintf("  mem uname dmesg handles crash yield\n");
}

static void cmd_ps(void)
{
    sys_process_info_t buf[MAX_PROCESSES];
    u64 got = 0;
    status_t st = NtQuerySystemInformation(5, buf, sizeof(buf), &got);
    if (!NT_SUCCESS(st)) {
        kprintf("ps: %s\n", status_name(st));
        return;
    }
    u32 n = (u32)(got / sizeof(buf[0]));
    kprintf("  PID  THR  STATE  IMAGE\n");
    for (u32 i = 0; i < n; i++) {
        kprintf("%5llu  %3u  %-5s  %s\n",
                (unsigned long long)buf[i].pid, buf[i].threads,
                buf[i].state, buf[i].image);
    }
}

static void cmd_mem(void)
{
    sys_mem_info_t m;
    u64 got = 0;
    status_t st = NtQuerySystemInformation(0, &m, sizeof(m), &got);
    if (!NT_SUCCESS(st)) {
        kprintf("mem: %s\n", status_name(st));
        return;
    }
    kprintf("pages total %llu free %llu  heap %llu  ticks %llu\n",
            (unsigned long long)m.total_pages,
            (unsigned long long)m.free_pages,
            (unsigned long long)m.heap_used,
            (unsigned long long)m.ticks);
}

static void cmd_ls(const char *path)
{
    handle_t h;
    const char *p = path ? path : ".";
    status_t st = NtCreateFile(&h, FILE_READ_DATA | DIRECTORY_QUERY, p, FILE_OPEN, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        kprintf("ls: %s: %s\n", p, status_name(st));
        return;
    }
    char buf[1024];
    st = NtQueryDirectoryFile(h, buf, sizeof(buf) - 1, true);
    if (st == STATUS_END_OF_FILE) {
        NtClose(h);
        return;
    }
    if (!NT_SUCCESS(st)) {
        kprintf("ls: %s\n", status_name(st));
        NtClose(h);
        return;
    }
    buf[sizeof(buf) - 1] = 0;
    kprintf("%s\n", buf);
    NtClose(h);
}

static void cmd_cat(const char *path)
{
    if (!path) { kprintf("cat: usage: cat file\n"); return; }
    handle_t h;
    status_t st = NtCreateFile(&h, FILE_READ_DATA, path, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        kprintf("cat: %s: %s\n", path, status_name(st));
        return;
    }
    char buf[256];
    for (;;) {
        u64 n = 0;
        st = NtReadFile(h, buf, sizeof(buf) - 1, (u64)-1, &n);
        if (st == STATUS_END_OF_FILE) break;
        if (!NT_SUCCESS(st)) {
            kprintf("cat: %s\n", status_name(st));
            break;
        }
        buf[n] = 0;
        kprintf("%s", buf);
        if (n == 0) break;
    }
    NtClose(h);
}

static void cmd_echo(int ac, char **av)
{
    for (int i = 1; i < ac; i++) {
        if (i > 1) kprintf(" ");
        kprintf("%s", av[i]);
    }
    kprintf("\n");
}

static void cmd_mkdir(const char *path)
{
    if (!path) { kprintf("mkdir: usage\n"); return; }
    handle_t h;
    status_t st = NtCreateFile(&h, FILE_READ_DATA, path, FILE_CREATE, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) kprintf("mkdir: %s\n", status_name(st));
    else NtClose(h);
}

static void cmd_touch(const char *path)
{
    if (!path) return;
    handle_t h;
    status_t st = NtCreateFile(&h, FILE_WRITE_DATA, path, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) kprintf("touch: %s\n", status_name(st));
    else NtClose(h);
}

static void cmd_write(const char *path, const char *text)
{
    if (!path || !text) return;
    handle_t h;
    status_t st = NtCreateFile(&h, FILE_WRITE_DATA, path, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        kprintf("write: %s\n", status_name(st));
        return;
    }
    u64 n = 0;
    NtWriteFile(h, text, strlen(text), 0, &n);
    NtClose(h);
}

static char line[256];
static u32  linelen;

static void prompt(void)
{
    char cwd[PATH_MAX];
    NtGetCwd(cwd, sizeof(cwd));
    kprintf("root@jasos:%s# ", cwd);
}

int sh_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("sh: Aegis shell. help for commands.\n");
    prompt();
#ifdef JASOS_HOST
    /* Batch: if stdin is not a tty, still read lines. */
#endif
    for (;;) {
        int c = serial_poll_char();
        if (c < 0) {
            NtYieldExecution();
#ifdef JASOS_HOST
            /* EOF */
            if (feof(stdin)) {
                kprintf("\nsh: eof\n");
                return 0;
            }
#endif
            continue;
        }
        if (c == '\r') c = '\n';
        if (c == '\n') {
            kprintf("\n");
            line[linelen] = 0;
            char *av[16];
            int ac = split(line, av, 16);
            if (ac > 0) {
                if (strcmp(av[0], "help") == 0) cmd_help();
                else if (strcmp(av[0], "ps") == 0) cmd_ps();
                else if (strcmp(av[0], "mem") == 0) cmd_mem();
                else if (strcmp(av[0], "ls") == 0) cmd_ls(ac > 1 ? av[1] : ".");
                else if (strcmp(av[0], "cat") == 0) cmd_cat(ac > 1 ? av[1] : NULL);
                else if (strcmp(av[0], "echo") == 0) cmd_echo(ac, av);
                else if (strcmp(av[0], "mkdir") == 0) cmd_mkdir(ac > 1 ? av[1] : NULL);
                else if (strcmp(av[0], "touch") == 0) cmd_touch(ac > 1 ? av[1] : NULL);
                else if (strcmp(av[0], "write") == 0) cmd_write(ac > 1 ? av[1] : NULL, ac > 2 ? av[2] : "");
                else if (strcmp(av[0], "rm") == 0) {
                    status_t st = vfs_unlink(ac > 1 ? av[1] : "");
                    if (!NT_SUCCESS(st)) kprintf("rm: %s\n", status_name(st));
                } else if (strcmp(av[0], "cd") == 0) {
                    status_t st = NtSetCwd(ac > 1 ? av[1] : "/");
                    if (!NT_SUCCESS(st)) kprintf("cd: %s\n", status_name(st));
                } else if (strcmp(av[0], "pwd") == 0) {
                    char cwd[PATH_MAX];
                    NtGetCwd(cwd, sizeof(cwd));
                    kprintf("%s\n", cwd);
                } else if (strcmp(av[0], "uname") == 0) {
                    kprintf("jasOS Aegis " JASOS_VERSION_STR " x86_64\n");
                } else if (strcmp(av[0], "yield") == 0) {
                    NtYieldExecution();
                } else if (strcmp(av[0], "crash") == 0) {
                    NtRaiseException(STATUS_ACCESS_VIOLATION);
                } else if (strcmp(av[0], "exit") == 0) {
                    return 0;
                } else {
                    kprintf("sh: %s: not found\n", av[0]);
                }
            }
            linelen = 0;
            prompt();
            continue;
        }
        if (c == 0x7f || c == '\b') {
            if (linelen) {
                linelen--;
                kprintf("\b \b");
            }
            continue;
        }
        if (linelen + 1 < sizeof(line) && c >= 32 && c < 127) {
            line[linelen++] = (char)c;
            kputchar((char)c);
        }
    }
}
