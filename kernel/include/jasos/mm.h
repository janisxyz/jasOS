#pragma once

#include <jasos/types.h>
#include <jasos/config.h>
#include <jasos/status.h>

#define PMM_INVALID ((phys_t)~0ULL)

#define PMM_ZERO     0x1u
#define PMM_KERNEL   0x2u
#define PMM_DMA      0x4u
#define PMM_USER     0x8u

typedef struct mmap_entry {
    phys_t base;
    u64    length;
    u32    type; /* 1 = available */
} mmap_entry_t;

typedef struct frame {
    u8     order;
    u8     state; /* 0 free, 1 tail, 2 kernel, 3 user, 4 reserved */
    u16    pad;
    u32    next_pfn;
} frame_t;

void  pmm_init(const mmap_entry_t *map, u32 count, phys_t kphys, u64 ksize);
phys_t pmm_alloc(u32 order, u32 flags);
void  pmm_free(phys_t p, u32 order);
u64   pmm_free_pages(void);
u64   pmm_total_pages(void);
void  pmm_dump(void);
void  pmm_enter_hhdm(void);
void *pmm_phys_to_virt(phys_t pa);

#define PTE_P    (1ULL << 0)
#define PTE_W    (1ULL << 1)
#define PTE_U    (1ULL << 2)
#define PTE_PWT  (1ULL << 3)
#define PTE_PCD  (1ULL << 4)
#define PTE_A    (1ULL << 5)
#define PTE_D    (1ULL << 6)
#define PTE_PS   (1ULL << 7)
#define PTE_G    (1ULL << 8)
#define PTE_NX   (1ULL << 63)
#define PTE_SW_COMMIT (1ULL << 9)

typedef struct vad {
    virt_t  start;
    virt_t  end;
    u32     prot;
    u32     type;
    u32     committed;
} vad_t;

struct process;

typedef struct aspace {
    phys_t     cr3_phys;
    spinlock_t lock;
    vad_t      vads[MAX_VADS];
    u32        vad_count;
    virt_t     brk;
    virt_t     stack_base;
#ifdef JASOS_HOST
    u8     *host_shadow[MAX_VADS];
#endif
} aspace_t;

void   vmm_init(phys_t kphys, u64 ksize);
void   vmm_aspace_init(aspace_t *as);
void   vmm_aspace_destroy(aspace_t *as);
status_t vmm_map(aspace_t *as, virt_t va, phys_t pa, u64 n_pages, u64 flags);
status_t vmm_unmap(aspace_t *as, virt_t va, u64 n_pages);
status_t vmm_alloc_user(struct process *p, virt_t *base, u64 size, u32 prot, u32 type);
status_t vmm_free_user(struct process *p, virt_t base, u64 size);
bool   vmm_probe_user(aspace_t *as, virt_t va, u64 n, bool write);
bool   vmm_handle_user_fault(aspace_t *as, virt_t va, bool write);
aspace_t *vmm_kernel_aspace(void);
status_t vmm_write_aspace(aspace_t *as, virt_t va, const void *src, u64 n);
status_t vmm_read_aspace(aspace_t *as, void *dst, virt_t va, u64 n);
status_t copyin(void *kdst, virt_t usrc, u64 n);
status_t copyout(virt_t udst, const void *ksrc, u64 n);
status_t copyinstr(char *kdst, virt_t usrc, u64 cap);
status_t vmm_map_kstack(u32 tid, u8 **out);
void     vmm_unmap_kstack(u32 tid);
status_t vmm_map_guarded_stack(virt_t base, u64 size, u8 **out);

void  *kalloc(usize n);
void  *kalloc_zero(usize n);
void   kfree(void *p);
void   heap_init(void);
u64    heap_used(void);
