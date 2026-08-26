#pragma once

#include <jasos/types.h>
#include <jasos/status.h>
#include <jasos/config.h>

#define SYS_NtClose                    0
#define SYS_NtDuplicateObject          1
#define SYS_NtCreateFile               2
#define SYS_NtReadFile                 3
#define SYS_NtWriteFile                4
#define SYS_NtQueryDirectoryFile       5
#define SYS_NtCreateProcess            6
#define SYS_NtCreateThread             7
#define SYS_NtTerminateProcess         8
#define SYS_NtTerminateThread          9
#define SYS_NtWaitForSingleObject      10
#define SYS_NtCreateEvent              11
#define SYS_NtSetEvent                 12
#define SYS_NtResetEvent               13
#define SYS_NtCreateMutex              14
#define SYS_NtReleaseMutex             15
#define SYS_NtQuerySystemInformation   16
#define SYS_NtQueryInformationProcess  17
#define SYS_NtYieldExecution           18
#define SYS_NtDelayExecution           19
#define SYS_NtQueryObject              20
#define SYS_NtCreateSection            21
#define SYS_NtMapViewOfSection         22
#define SYS_NtUnmapViewOfSection       23
#define SYS_NtAllocateVirtualMemory    24
#define SYS_NtFreeVirtualMemory        25
#define SYS_NtQueryVirtualMemory       26
#define SYS_NtCreateDirectoryObject    27
#define SYS_NtOpenDirectoryObject      28
#define SYS_NtRaiseException           29
#define SYS_NtGetCwd                   30
#define SYS_NtSetCwd                   31
#define SYS_NtCreatePipe               32
#define SYS_NtCreateTimer              33
#define SYS_NtSetTimer                 34
#define SYS_NtCancelTimer              35
#define SYS_NtWaitForMultipleObjects   36
#define SYS_MAX                        37

status_t syscall_dispatch(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 *info);

/* Direct calls used by kernel-linked userland and host programs. */
status_t NtClose(handle_t h);
status_t NtCreateFile(handle_t *out, access_t access, const char *path, u32 disp, u32 opts);
status_t NtReadFile(handle_t h, void *buf, u64 n, u64 off, u64 *got);
status_t NtWriteFile(handle_t h, const void *buf, u64 n, u64 off, u64 *put);
status_t NtQueryDirectoryFile(handle_t h, void *buf, u64 n, bool restart);
status_t NtTerminateProcess(handle_t h, status_t st);
status_t NtYieldExecution(void);
status_t NtDelayExecution(u64 ticks);
status_t NtQuerySystemInformation(u32 cls, void *buf, u64 n, u64 *got);
status_t NtSetCwd(const char *path);
status_t NtGetCwd(char *buf, u64 cap);
status_t NtCreateEvent(handle_t *out, const char *name, bool auto_reset, bool initial);
status_t NtSetEvent(handle_t h);
status_t NtCreateMutex(handle_t *out, const char *name, bool owner);
status_t NtReleaseMutex(handle_t h);
status_t NtWaitForSingleObject(handle_t h, u64 timeout_ticks);
status_t NtRaiseException(status_t code);
status_t NtResetEvent(handle_t h);
status_t NtCreateThread(handle_t *out, void (*entry)(void *), void *arg, u32 prio);
status_t NtAllocateVirtualMemory(handle_t proc, virt_t *base, u64 size, u32 prot);
status_t NtCreatePipe(handle_t *read_out, handle_t *write_out);
status_t NtQueryInformationProcess(handle_t h, void *buf, u64 n);
status_t NtCreateProcess(handle_t *out, access_t access, const char *image, u32 flags);
status_t NtTerminateThread(handle_t h, status_t st);
status_t NtDuplicateObject(handle_t src_proc, handle_t src, handle_t dst_proc, handle_t *out, access_t access, u32 flags);
status_t NtQueryObject(handle_t h, void *buf, u64 n);
status_t NtCreateSection(handle_t *out, access_t access, u64 size, u32 prot);
status_t NtMapViewOfSection(handle_t section, handle_t proc, virt_t *base, u64 size, u32 prot);
status_t NtUnmapViewOfSection(handle_t proc, virt_t base);
status_t NtFreeVirtualMemory(handle_t proc, virt_t base, u64 size);
status_t NtCreateDirectoryObject(handle_t *out, const char *name);
status_t NtOpenDirectoryObject(handle_t *out, const char *path);
status_t NtCreateTimer(handle_t *out, const char *name, bool auto_reset);
status_t NtSetTimer(handle_t h, u64 due_ticks, u64 period);
status_t NtCancelTimer(handle_t h);
status_t NtWaitForMultipleObjects(handle_t *hs, u32 count, bool wait_all, u64 timeout_ticks);
status_t NtQueryVirtualMemory(handle_t proc, virt_t addr, void *buf, u64 n);

typedef struct sys_process_info {
    kpid_t pid;
    u32   threads;
    char  image[64];
    char  state[16];
} sys_process_info_t;

typedef struct sys_mem_info {
    u64 total_pages;
    u64 free_pages;
    u64 heap_used;
    u64 ticks;
} sys_mem_info_t;

typedef struct object_basic_information {
    u32  kind;
    u64  pointer_count;
    u64  handle_count;
    char type_name[32];
    char name[NAME_MAX];
} object_basic_information_t;

typedef struct memory_basic_information {
    virt_t base;
    virt_t alloc_base;
    u64    region_size;
    u32    prot;
    u32    type;
    u32    state;
} memory_basic_information_t;
