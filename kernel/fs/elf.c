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
 *  - W^X PT_LOAD and PF_X PT_GNU_STACK are refused. Missing
 *    GNU_STACK defaults to NX (we do not replay Linux's exec-stack
 *    heritage).
 */

#define EI_NIDENT 16
#define ELFMAG0 0x7f
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_GNU_STACK 0x6474E551u
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
    /* Policy pass. Do not leave a partial map because phdr N+1 is illegal. */
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP || ph[i].p_type == PT_DYNAMIC)
            return STATUS_NOT_SUPPORTED;
        if (ph[i].p_type == PT_GNU_STACK) {
            if (ph[i].p_flags & PF_X)
                return STATUS_INVALID_IMAGE_FORMAT;
            continue;
        }
        if (ph[i].p_type != PT_LOAD) continue;
        if ((ph[i].p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
            return STATUS_INVALID_IMAGE_FORMAT;
        if (ph[i].p_vaddr > USER_CANONICAL_TOP) return STATUS_ACCESS_VIOLATION;
        if (ph[i].p_memsz < ph[i].p_filesz) return STATUS_INVALID_IMAGE_FORMAT;
        if (ph[i].p_offset + ph[i].p_filesz > len) return STATUS_INVALID_IMAGE_FORMAT;
    }
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        virt_t va = PAGE_ALIGN_DOWN(ph[i].p_vaddr);
        u64 size = PAGE_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz) - va;
        u32 prot = PAGE_READONLY;
        if (ph[i].p_flags & PF_W) prot = PAGE_READWRITE;
        if (ph[i].p_flags & PF_X) prot = PAGE_EXECUTE_READ;
        virt_t base = va;
        status_t st = vmm_alloc_user(p, &base, size, prot, MEM_COMMIT);
        if (!NT_SUCCESS(st)) return st;
        if (ph[i].p_filesz) {
            st = vmm_write_aspace(&p->aspace, ph[i].p_vaddr,
                                  image + ph[i].p_offset, ph[i].p_filesz);
            if (!NT_SUCCESS(st)) return st;
        }
    }
    if (entry_out) *entry_out = eh->e_entry;
    return STATUS_SUCCESS;
}

/*
 * Tiny ET_EXEC used by host selftest so ELF coverage does not depend
 * on the userland toolchain having already run.
 */
u64 elf_make_minimal_hello(u8 *out, u64 cap)
{
    if (!out || cap < 0x80) return 0;
    memset(out, 0, (size_t)cap);
    /* e_ident */
    out[0] = 0x7f; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
    out[4] = 2;    out[5] = 1;   out[6] = 1;
    /* e_type ET_EXEC, e_machine EM_X86_64, e_version 1 */
    out[16] = 2; out[17] = 0;
    out[18] = 62; out[19] = 0;
    out[20] = 1; out[21] = 0; out[22] = 0; out[23] = 0;
    /* e_entry = 0x400000 */
    out[24] = 0x00; out[25] = 0x00; out[26] = 0x40; out[27] = 0x00;
    /* e_phoff = 64 */
    out[32] = 64;
    /* e_ehsize = 64, e_phentsize = 56, e_phnum = 1 */
    out[52] = 64; out[53] = 0;
    out[54] = 56; out[55] = 0;
    out[56] = 1;  out[57] = 0;
    /* phdr at 64: PT_LOAD, PF_R|PF_X, offset 0, vaddr 0x400000,
       filesz/memsz 0x80, align 0x1000 */
    out[64] = 1; /* PT_LOAD */
    out[68] = 5; /* PF_R|PF_X */
    out[80] = 0x00; out[81] = 0x00; out[82] = 0x40; out[83] = 0x00; /* vaddr */
    out[88] = 0x00; out[89] = 0x00; out[90] = 0x40; out[91] = 0x00; /* paddr */
    out[96] = 0x80; /* filesz */
    out[104] = 0x80; /* memsz */
    out[112] = 0x00; out[113] = 0x10; /* align 0x1000 */
    /* code at file offset 0 is overlapping the header — that's legal.
       Put `ret` at entry 0x400000 which is file offset 0. Overwriting
       EI_MAG is fatal, so put a NOP sled at 0x400000 by using entry
       at 0x400040 (file offset 0x40 is inside the phdr). Simpler:
       filesz covers header+code; entry at 0x400070 = offset 0x70. */
    out[24] = 0x70; /* e_entry = 0x400070 */
    out[0x70] = 0xC3; /* ret */
    return 0x80;
}
