#include <jasos/kprintf.h>

int echo_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) kprintf(" ");
        kprintf("%s", argv[i]);
    }
    kprintf("\n");
    return 0;
}
