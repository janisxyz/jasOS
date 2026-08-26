#include <jasos/mm.h>
#include <jasos/ke.h>
#include <jasos/fs.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>

/*
 * ELF64 ET_EXEC loader. Not a toy: we validate class, endian, machine,
 * phentsize, and refuse PT_DYNAMIC (no interpreter in v1). File-backed
 * sections are copied, not demand-paged (MEMORY.md).
 *
 * Why this will fail in production:
 *  - No ASLR. Image is at the ELF vaddr.
 *  - No relocations. ET_DYN is STATUS_NOT_SUPPORTED.
 *  - Interpreter (PT_INTERP) is refused — static only.
 */

#define EI_NIDENT 16
#define ELFMAG0 0x7f
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct PACKED {
    u8  e_ident[EI_NIDENT];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr;

typedef struct PACKED {
    u32 p_type, p_flags;
    u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} elf64_phdr;

status_t elf_load(process_t *p, const u8 *image, u64 len, virt_t *entry_out)
{
    if (!p || !image || len < sizeof(elf64_ehdr)) return STATUS_INVALID_IMAGE_FORMAT;
    const elf64_ehdr *eh = (const elf64_ehdr *)image;
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return STATUS_INVALID_IMAGE_FORMAT;
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (eh->e_machine != EM_X86_64) return STATUS_INVALID_IMAGE_FORMAT;
    if (eh->e_type == ET_DYN) return STATUS_NOT_SUPPORTED;
    if (eh->e_type != ET_EXEC) return STATUS_INVALID_IMAGE_FORMAT;
    if (eh->e_phentsize != sizeof(elf64_phdr) || eh->e_phnum == 0 || eh->e_phnum > 32)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (eh->e_phoff + (u64)eh->e_phnum * sizeof(elf64_phdr) > len)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (eh->e_entry > USER_CANONICAL_TOP) return STATUS_INVALID_IMAGE_FORMAT;

    const elf64_phdr *ph = (const elf64_phdr *)(image + eh->e_phoff);
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP || ph[i].p_type == PT_DYNAMIC)
            return STATUS_NOT_SUPPORTED;
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr > USER_CANONICAL_TOP) return STATUS_ACCESS_VIOLATION;
        if (ph[i].p_memsz < ph[i].p_filesz) return STATUS_INVALID_IMAGE_FORMAT;
        if (ph[i].p_offset + ph[i].p_filesz > len) return STATUS_INVALID_IMAGE_FORMAT;
        virt_t va = PAGE_ALIGN_DOWN(ph[i].p_vaddr);
        u64 size = PAGE_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz) - va;
        u32 prot = PAGE_READONLY;
        if (ph[i].p_flags & PF_W) prot = PAGE_READWRITE;
        if (ph[i].p_flags & PF_X) {
            prot = (ph[i].p_flags & PF_W) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        }
        virt_t base = va;
        status_t st = vmm_alloc_user(p, &base, size, prot, MEM_COMMIT);
        if (!NT_SUCCESS(st)) return st;
#ifdef JASOS_HOST
        memcpy((void *)(uintptr_t)ph[i].p_vaddr, image + ph[i].p_offset, (size_t)ph[i].p_filesz);
#else
        /* Hardware: the pages are mapped; copy through HHDM of the phys
           backing. v1 copies via the mapped user VA after the map is in
           the *current* aspace — loader must run in the target process
           or we need MmCopyVirtualMemory. We run in-process. */
        memcpy((void *)(uintptr_t)ph[i].p_vaddr, image + ph[i].p_offset, (size_t)ph[i].p_filesz);
#endif
    }
    if (entry_out) *entry_out = eh->e_entry;
    return STATUS_SUCCESS;
}
