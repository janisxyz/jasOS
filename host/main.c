#include <jasos/ke.h>
#include <jasos/mm.h>
#include <jasos/kprintf.h>
#include <jasos/config.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int g_selftest;
static int g_shell;

int host_wants_selftest(void) { return g_selftest; }
int host_wants_shell(void) { return g_shell; }

void host_build_mmap(mmap_entry_t *map, u32 *count)
{
    memset(map, 0, sizeof(*map) * 4);
    map[0].base = 0;
    map[0].length = 128ull * 1024ull * 1024ull;
    map[0].type = 1;
    *count = 1;
}

void kmain_early(u64 mb2);

int main(int argc, char **argv)
{
    g_selftest = 1;
    g_shell = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) { g_selftest = 1; g_shell = 0; }
        else if (strcmp(argv[i], "--shell") == 0) { g_selftest = 0; g_shell = 1; }
        else if (strcmp(argv[i], "--boot") == 0) { g_selftest = 0; g_shell = 0; }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "jasos-host [--selftest|--shell|--boot]\n");
            return 0;
        }
    }
    /* Default with no args: selftest, matching `make host`. */
    if (argc == 1) g_selftest = 1;
    kmain_early(0);
    return 0;
}
