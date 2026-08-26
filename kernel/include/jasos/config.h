#pragma once

#include <jasos/types.h>

#define JASOS_VERSION_MAJOR  0
#define JASOS_VERSION_MINOR  12
#define JASOS_VERSION_PATCH  0
#define JASOS_VERSION_STR    "0.12.0-aegis"


#define PAGE_SHIFT           12u
#define PAGE_SIZE            (1u << PAGE_SHIFT)
#define PAGE_MASK            (PAGE_SIZE - 1u)
#define PAGE_ALIGN_DOWN(x)   ((u64)(x) & ~(u64)PAGE_MASK)
#define PAGE_ALIGN_UP(x)     (((u64)(x) + PAGE_MASK) & ~(u64)PAGE_MASK)

#define KERNEL_VMA           0xFFFFFFFF80000000ULL
#define HHDM_BASE            0xFFFF800000000000ULL
#define HEAP_BASE            0xFFFF900000000000ULL
#define RECURSIVE_SLOT       510u
#define KERNEL_PML4_START    256u

#define USER_CANONICAL_TOP   0x00007FFFFFFFFFFFULL
#define USER_IMAGE_BASE      0x0000000000400000ULL
#define USER_HEAP_BASE       0x0000000001000000ULL
#define USER_STACK_TOP       0x00007FFFFFF00000ULL
#define USER_STACK_SIZE      (128u * 1024u)

#define KSTACK_SIZE          (16u * 1024u)
#define KSTACK_GUARD         PAGE_SIZE
/* 32 KiB VA per thread: 4 KiB not-present guard, 16 KiB stack, 12 KiB hole. */
#define KSTACK_STRIDE        (32u * 1024u)
#define BOOT_STACK_SIZE      (16u * 1024u)

#define PMM_MAX_ORDER        18u
#define PMM_ORDERS           (PMM_MAX_ORDER + 1u)

#define HEAP_CLASSES         9u

#define HANDLE_TABLE_SLOTS   1024u
/* handle = (generation << 16) | (index << 2). Low 2 bits stay 0. */
#define HANDLE_VALUE(i, g)   (((handle_t)(i) << 2) | ((handle_t)(u32)(g) << 16))
#define HANDLE_INDEX(h)      ((u32)(((h) >> 2) & 0x3FFFu))
#define HANDLE_GEN(h)        ((u32)((h) >> 16))
#define HANDLE_CURRENT       ((handle_t)-1)

#define PRIORITY_LEVELS      32u
#define PRIORITY_IDLE        0u
#define PRIORITY_NORMAL      8u
#define PRIORITY_HIGH        16u
#define PRIORITY_REALTIME    24u
#define QUANTUM_TICKS        3u
#define TIMER_HZ             100u

#define PATH_MAX             512u
#define NAME_MAX             64u
#define PATH_DEPTH_MAX       32u
#define COPY_MAX             (1u * 1024u * 1024u)
#define SYSCALL_COPY_MAX     (64u * 1024u)

#define WAIT_OBJECTS_MAX     16u
#define WAIT_ANY             0u
#define WAIT_ALL             1u

#define KERNEL_STACK_BASE    0xFFFFA00000000000ULL


#define MAX_CPUS             1u
#define MAX_PROCESSES        64u
#define MAX_THREADS          256u
#define MAX_VADS             64u
#define LOCK_DEPTH_MAX       8u
#define IST_STACK_BASE       (KERNEL_STACK_BASE + (u64)MAX_THREADS * KSTACK_STRIDE)

#define SERIAL_COM1          0x3F8u
#define SERIAL_BAUD          115200u

/* 1 MiB ramdisk. Fixed size; writes past the end fail closed. */
#define RAMDISK_SIZE         (1u * 1024u * 1024u)
#define RAMDISK_SECTOR       512u

/* Per-process committed user virtual. A mapping bomb fails here, not in PMM. */
#define USER_COMMIT_MAX      (32u * 1024u * 1024u)

/*
 * Lock ranking. Acquire only a STRICTLY HIGHER rank while holding one.
 * Nested ranks are stacked on the PCB; unlock restores the previous.
 *
 * DISP < SCHED is mandatory: wait/signal holds the object dispatcher
 * then inserts the woken thread on a ready queue.
 *
 * T3 reversal: v0.3 had SCHED=8 DISP=9. ping/pong paniced
 * "lock rank 8 (sched) while holding 9". Swapped and stacked.
 */
#define LOCK_RANK_PANIC      0u
#define LOCK_RANK_SERIAL     1u
#define LOCK_RANK_PMM        2u
#define LOCK_RANK_HEAP       3u
#define LOCK_RANK_VMM        4u
#define LOCK_RANK_VAD        5u
#define LOCK_RANK_OB         6u
#define LOCK_RANK_HANDLE     7u
#define LOCK_RANK_PROC       8u
#define LOCK_RANK_DISP       9u
#define LOCK_RANK_SCHED      10u
#define LOCK_RANK_VFS        11u

#define IRQL_PASSIVE         0u
#define IRQL_APC             1u
#define IRQL_DISPATCH        2u
#define IRQL_DEVICE          3u
#define IRQL_HIGH            4u

#define GENERIC_READ         0x80000000u
#define GENERIC_WRITE        0x40000000u
#define GENERIC_EXECUTE      0x20000000u
#define GENERIC_ALL          0x10000000u
#define DELETE               0x00010000u
#define READ_CONTROL         0x00020000u
#define WRITE_DAC            0x00040000u
#define SYNCHRONIZE          0x00100000u

#define PROCESS_TERMINATE        0x0001u
#define PROCESS_CREATE_THREAD    0x0002u
#define PROCESS_VM_OPERATION     0x0008u
#define PROCESS_VM_READ          0x0010u
#define PROCESS_VM_WRITE         0x0020u
#define PROCESS_DUP_HANDLE       0x0040u
#define PROCESS_QUERY_INFORMATION 0x0400u
#define PROCESS_ALL_ACCESS       0x1FFFFFu

#define THREAD_TERMINATE         0x0001u
#define THREAD_SUSPEND_RESUME    0x0002u
#define THREAD_QUERY_INFORMATION 0x0040u
#define THREAD_ALL_ACCESS        0x1FFFFFu

#define FILE_READ_DATA           0x0001u
#define FILE_WRITE_DATA          0x0002u
#define FILE_APPEND_DATA         0x0004u
#define FILE_EXECUTE             0x0020u
#define FILE_READ_ATTRIBUTES     0x0080u
#define FILE_WRITE_ATTRIBUTES    0x0100u
#define FILE_ALL_ACCESS          0x1FFFFFu

#define DIRECTORY_QUERY          0x0001u
#define DIRECTORY_TRAVERSE       0x0002u
#define DIRECTORY_CREATE_OBJECT  0x0004u

#define EVENT_MODIFY_STATE       0x0002u
#define MUTEX_MODIFY_STATE       0x0001u
#define TIMER_MODIFY_STATE       0x0002u
#define SECTION_MAP_WRITE        0x0002u
#define SECTION_MAP_READ         0x0004u
#define SECTION_MAP_EXECUTE      0x0008u
#define SECTION_ALL_ACCESS       0x1FFFFu

#define FILE_OPEN                1u
#define FILE_CREATE              2u
#define FILE_OPEN_IF             3u
#define FILE_OVERWRITE           4u
#define FILE_DIRECTORY_FILE      0x00000001u
#define FILE_NON_DIRECTORY_FILE  0x00000040u

#define MEM_COMMIT               0x1000u
#define MEM_RESERVE              0x2000u
#define MEM_RELEASE              0x8000u
#define PAGE_NOACCESS            0x01u
#define PAGE_READONLY            0x02u
#define PAGE_READWRITE           0x04u
#define PAGE_EXECUTE             0x10u
#define PAGE_EXECUTE_READ        0x20u
#define PAGE_EXECUTE_READWRITE   0x40u

#define CREATE_SUSPENDED         0x00000001u
#define CREATE_NO_IMAGE          0x00000002u

#define DUPLICATE_CLOSE_SOURCE   0x00000001u
#define DUPLICATE_SAME_ACCESS    0x00000002u
#define DUPLICATE_INHERIT        0x00000004u
