#include <jasos/syscall.h>
#include <jasos/config.h>

void *malloc(unsigned long n)
{
    if (n == 0) n = 1;
    virt_t base = 0;
    u64 sz = PAGE_ALIGN_UP(n + 16);
    if (!NT_SUCCESS(NtAllocateVirtualMemory(HANDLE_CURRENT, &base, sz, PAGE_READWRITE)))
        return 0;
    *(u64 *)(uintptr_t)base = sz;
    return (void *)(uintptr_t)(base + 16);
}

void free(void *p)
{
    if (!p) return;
    virt_t hdr = (virt_t)(uintptr_t)p - 16;
    u64 sz = *(u64 *)(uintptr_t)hdr;
    NtFreeVirtualMemory(HANDLE_CURRENT, hdr, sz);
}
