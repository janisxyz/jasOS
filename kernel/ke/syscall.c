#include <jasos/syscall.h>
#include <jasos/ke.h>
#include <jasos/fs.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>

static handle_table_t *cur_ht(void)
{
    process_t *p = ke_current_process();
    return p ? &p->handles : NULL;
}

status_t NtClose(handle_t h)
{
    handle_table_t *t = cur_ht();
    if (!t) return STATUS_INVALID_HANDLE;
    return ht_close(t, h);
}

status_t NtCreateFile(handle_t *out, access_t access, const char *path, u32 disp, u32 opts)
{
    if (!out || !path) return STATUS_INVALID_PARAMETER;
    file_object_t *f;
    status_t st = vfs_open(path, access, disp, opts, &f);
    if (!NT_SUCCESS(st)) return st;
    st = ht_insert(cur_ht(), &f->hdr, access, out);
    ob_dereference(&f->hdr);
    return st;
}

status_t NtReadFile(handle_t h, void *buf, u64 n, u64 off, u64 *got)
{
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, FILE_READ_DATA, 0, &o);
    if (!NT_SUCCESS(st)) return st;
    if (o->type->kind == OBJ_PIPE) {
        st = pipe_read(o, buf, n, got);
        ob_dereference(o);
        return st;
    }
    file_object_t *f = (file_object_t *)o;
    if (off != (u64)-1) f->offset = off;
    st = vfs_read(f, buf, n, got);
    ob_dereference(o);
    return st;
}

status_t NtWriteFile(handle_t h, const void *buf, u64 n, u64 off, u64 *put)
{
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, 0, 0, &o);
    if (!NT_SUCCESS(st)) return st;
    if (o->type->kind == OBJ_PIPE) {
        st = pipe_write(o, buf, n, put);
        ob_dereference(o);
        return st;
    }
    file_object_t *f = (file_object_t *)o;
    if (off != (u64)-1) f->offset = off;
    st = vfs_write(f, buf, n, put);
    ob_dereference(o);
    return st;
}

status_t NtQueryDirectoryFile(handle_t h, void *buf, u64 n, bool restart)
{
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, FILE_READ_DATA | DIRECTORY_QUERY, 0, &o);
    if (!NT_SUCCESS(st)) {
        st = ht_lookup(cur_ht(), h, 0, OBJ_FILE, &o);
        if (!NT_SUCCESS(st)) return st;
    }
    file_object_t *f = (file_object_t *)o;
    u64 put = 0;
    st = vfs_readdir(f, buf, n, &put, restart);
    ob_dereference(o);
    return st;
}

status_t NtTerminateProcess(handle_t h, status_t stcode)
{
    process_t *p;
    if (h == HANDLE_CURRENT) p = ke_current_process();
    else {
        object_t *o;
        status_t st = ht_lookup(cur_ht(), h, PROCESS_TERMINATE, OBJ_PROCESS, &o);
        if (!NT_SUCCESS(st)) return st;
        p = (process_t *)o;
        ob_dereference(o);
    }
    if (!p) return STATUS_INVALID_HANDLE;
    if (p->pid == 1) panic("init died status %s", status_name(stcode));
    p->terminating = true;
    p->exit_status = stcode;
    sched_exit_thread(stcode);
    return STATUS_SUCCESS;
}

status_t NtYieldExecution(void)
{
    sched_yield();
    return STATUS_SUCCESS;
}

status_t NtDelayExecution(u64 ticks)
{
    if (ticks == 0) {
        sched_yield();
        return STATUS_SUCCESS;
    }
    timer_object_t *t;
    status_t st = ob_create_timer(NULL, true, &t);
    if (!NT_SUCCESS(st)) return st;
    ke_set_timer(t, ticks, 0);
    st = ke_wait_object(&t->disp, ticks + 2);
    ke_cancel_timer(t);
    ob_dereference(&t->hdr);
    return (st == STATUS_TIMEOUT) ? STATUS_SUCCESS : st;
}

extern u32 psp_snapshot(sys_process_info_t *buf, u32 max);

status_t NtQuerySystemInformation(u32 cls, void *buf, u64 n, u64 *got)
{
    if (!buf) return STATUS_INVALID_PARAMETER;
    if (cls == 0) {
        if (n < sizeof(sys_mem_info_t)) return STATUS_INFO_LENGTH_MISMATCH;
        sys_mem_info_t *m = buf;
        m->total_pages = pmm_total_pages();
        m->free_pages = pmm_free_pages();
        m->heap_used = heap_used();
        m->ticks = ke_ticks();
        if (got) *got = sizeof(*m);
        return STATUS_SUCCESS;
    }
    if (cls == 5) {
        u32 max = (u32)(n / sizeof(sys_process_info_t));
        u32 c = psp_snapshot(buf, max);
        if (got) *got = c * sizeof(sys_process_info_t);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_INFO_CLASS;
}

status_t NtSetCwd(const char *path)
{
    process_t *p = ke_current_process();
    if (!p || !path) return STATUS_INVALID_PARAMETER;
    vnode_t *v;
    status_t st = vfs_stat_path(path, &v);
    if (!NT_SUCCESS(st)) return st;
    if (v->kind != VNODE_DIR) return STATUS_NOT_A_DIRECTORY;
    char abs[PATH_MAX];
    st = path_norm(p->cwd, path, abs, PATH_MAX);
    if (!NT_SUCCESS(st)) return st;
    strlcpy(p->cwd, abs, PATH_MAX);
    return STATUS_SUCCESS;
}

status_t NtGetCwd(char *buf, u64 cap)
{
    process_t *p = ke_current_process();
    if (!p || !buf || cap == 0) return STATUS_INVALID_PARAMETER;
    strlcpy(buf, p->cwd, (usize)cap);
    return STATUS_SUCCESS;
}

status_t NtCreateEvent(handle_t *out, const char *name, bool auto_reset, bool initial)
{
    event_object_t *e;
    status_t st = ob_create_event(name, auto_reset, initial, &e);
    if (!NT_SUCCESS(st)) return st;
    st = ht_insert(cur_ht(), &e->hdr, SYNCHRONIZE | EVENT_MODIFY_STATE, out);
    ob_dereference(&e->hdr);
    return st;
}

status_t NtSetEvent(handle_t h)
{
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, EVENT_MODIFY_STATE, OBJ_EVENT, &o);
    if (!NT_SUCCESS(st)) return st;
    st = ke_set_event((event_object_t *)o);
    ob_dereference(o);
    return st;
}

status_t NtCreateMutex(handle_t *out, const char *name, bool owner)
{
    mutex_object_t *m;
    status_t st = ob_create_mutex(name, owner, &m);
    if (!NT_SUCCESS(st)) return st;
    st = ht_insert(cur_ht(), &m->hdr, SYNCHRONIZE | MUTEX_MODIFY_STATE, out);
    ob_dereference(&m->hdr);
    return st;
}

status_t NtReleaseMutex(handle_t h)
{
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, MUTEX_MODIFY_STATE, OBJ_MUTEX, &o);
    if (!NT_SUCCESS(st)) return st;
    st = ke_release_mutex((mutex_object_t *)o);
    ob_dereference(o);
    return st;
}

status_t NtWaitForSingleObject(handle_t h, u64 timeout_ticks)
{
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, SYNCHRONIZE, 0, &o);
    if (!NT_SUCCESS(st)) {
        st = ht_lookup(cur_ht(), h, 0, 0, &o);
        if (!NT_SUCCESS(st)) return st;
    }
    if (!o->wait) {
        ob_dereference(o);
        return STATUS_INVALID_PARAMETER;
    }
    st = ke_wait_object(o->wait, timeout_ticks);
    ob_dereference(o);
    return st;
}

status_t NtRaiseException(status_t code)
{
    kprintf("NtRaiseException %s in %s\n", status_name(code),
            ke_current() ? ke_current()->name : "?");
    sched_exit_thread(code);
    return code;
}

status_t NtResetEvent(handle_t h)
{
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, EVENT_MODIFY_STATE, OBJ_EVENT, &o);
    if (!NT_SUCCESS(st)) return st;
    st = ke_reset_event((event_object_t *)o);
    ob_dereference(o);
    return st;
}

status_t NtCreateThread(handle_t *out, void (*entry)(void *), void *arg, u32 prio)
{
    if (!out || !entry) return STATUS_INVALID_PARAMETER;
    process_t *p = ke_current_process();
    if (!p) return STATUS_INVALID_PARAMETER;
    thread_t *t;
    status_t st = psp_create_thread(p, "user", entry, arg, prio, &t);
    if (!NT_SUCCESS(st)) return st;
    return ht_insert(&p->handles, &t->hdr, THREAD_ALL_ACCESS, out);
}

status_t NtAllocateVirtualMemory(handle_t proc, virt_t *base, u64 size, u32 prot)
{
    process_t *p;
    if (proc == HANDLE_CURRENT) p = ke_current_process();
    else {
        object_t *o;
        status_t st = ht_lookup(cur_ht(), proc, PROCESS_VM_OPERATION, OBJ_PROCESS, &o);
        if (!NT_SUCCESS(st)) return st;
        p = (process_t *)o;
        ob_dereference(o);
    }
    if (!p || !base || size == 0) return STATUS_INVALID_PARAMETER;
    return vmm_alloc_user(p, base, size, prot ? prot : PAGE_READWRITE, MEM_COMMIT);
}

status_t NtQueryInformationProcess(handle_t h, void *buf, u64 n)
{
    process_t *p;
    if (h == HANDLE_CURRENT) p = ke_current_process();
    else {
        object_t *o;
        status_t st = ht_lookup(cur_ht(), h, PROCESS_QUERY_INFORMATION, OBJ_PROCESS, &o);
        if (!NT_SUCCESS(st)) return st;
        p = (process_t *)o;
        ob_dereference(o);
    }
    if (!p || !buf || n < sizeof(sys_process_info_t)) return STATUS_INVALID_PARAMETER;
    sys_process_info_t *inf = buf;
    inf->pid = p->pid;
    inf->threads = p->thread_count;
    strlcpy(inf->image, p->image, sizeof(inf->image));
    strlcpy(inf->state, p->terminating ? "exit" : "run", sizeof(inf->state));
    return STATUS_SUCCESS;
}

status_t syscall_from_entry(u64 *frame)
{
    /* nr, a0, a1, a2, a3, a4, a5 */
    u64 info = 0;
    status_t st = syscall_dispatch(frame[0], frame[1], frame[2], frame[3],
                                   frame[4], frame[5], frame[6], &info);
    return st;
}

status_t syscall_dispatch(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 *info)
{
    (void)a5;
    if (info) *info = 0;
    switch (nr) {
    case SYS_NtClose:              return NtClose((handle_t)a0);
    case SYS_NtCreateFile:         return NtCreateFile((handle_t *)a0, (access_t)a1, (const char *)a2, (u32)a3, (u32)a4);
    case SYS_NtReadFile:           return NtReadFile((handle_t)a0, (void *)a1, a2, a3, (u64 *)a4);
    case SYS_NtWriteFile:          return NtWriteFile((handle_t)a0, (const void *)a1, a2, a3, (u64 *)a4);
    case SYS_NtQueryDirectoryFile: return NtQueryDirectoryFile((handle_t)a0, (void *)a1, a2, a3 != 0);
    case SYS_NtTerminateProcess:   return NtTerminateProcess((handle_t)a0, (status_t)a1);
    case SYS_NtYieldExecution:     return NtYieldExecution();
    case SYS_NtDelayExecution:     return NtDelayExecution(a0);
    case SYS_NtQuerySystemInformation: return NtQuerySystemInformation((u32)a0, (void *)a1, a2, (u64 *)a3);
    case SYS_NtSetCwd:             return NtSetCwd((const char *)a0);
    case SYS_NtGetCwd:             return NtGetCwd((char *)a0, a1);
    case SYS_NtCreateEvent:        return NtCreateEvent((handle_t *)a0, (const char *)a1, a2 != 0, a3 != 0);
    case SYS_NtSetEvent:           return NtSetEvent((handle_t)a0);
    case SYS_NtResetEvent:         return NtResetEvent((handle_t)a0);
    case SYS_NtCreateMutex:        return NtCreateMutex((handle_t *)a0, (const char *)a1, a2 != 0);
    case SYS_NtReleaseMutex:       return NtReleaseMutex((handle_t)a0);
    case SYS_NtWaitForSingleObject:return NtWaitForSingleObject((handle_t)a0, a1);
    case SYS_NtRaiseException:     return NtRaiseException((status_t)a0);
    case SYS_NtCreateThread:       return NtCreateThread((handle_t *)a0, (void (*)(void *))a1, (void *)a2, (u32)a3);
    case SYS_NtAllocateVirtualMemory: return NtAllocateVirtualMemory((handle_t)a0, (virt_t *)a1, a2, (u32)a3);
    case SYS_NtQueryInformationProcess: return NtQueryInformationProcess((handle_t)a0, (void *)a1, a2);
    case SYS_NtCreatePipe:         return NtCreatePipe((handle_t *)a0, (handle_t *)a1);
    case SYS_NtCreateProcess:      return NtCreateProcess((handle_t *)a0, (access_t)a1, (const char *)a2, (u32)a3);
    case SYS_NtTerminateThread:    return NtTerminateThread((handle_t)a0, (status_t)a1);
    case SYS_NtDuplicateObject:    return NtDuplicateObject((handle_t)a0, (handle_t)a1, (handle_t)a2, (handle_t *)a3, (access_t)a4);
    case SYS_NtQueryObject:        return NtQueryObject((handle_t)a0, (void *)a1, a2);
    case SYS_NtCreateSection:      return NtCreateSection((handle_t *)a0, (access_t)a1, a2, (u32)a3);
    case SYS_NtMapViewOfSection:   return NtMapViewOfSection((handle_t)a0, (handle_t)a1, (virt_t *)a2, a3, (u32)a4);
    case SYS_NtUnmapViewOfSection: return NtUnmapViewOfSection((handle_t)a0, (virt_t)a1);
    case SYS_NtFreeVirtualMemory:  return NtFreeVirtualMemory((handle_t)a0, (virt_t)a1, a2);
    case SYS_NtCreateDirectoryObject: return NtCreateDirectoryObject((handle_t *)a0, (const char *)a1);
    case SYS_NtOpenDirectoryObject:   return NtOpenDirectoryObject((handle_t *)a0, (const char *)a1);
    case SYS_NtCreateTimer:        return NtCreateTimer((handle_t *)a0, (const char *)a1, a2 != 0);
    case SYS_NtSetTimer:           return NtSetTimer((handle_t)a0, a1, a2);
    case SYS_NtCancelTimer:        return NtCancelTimer((handle_t)a0);
    default:                       return STATUS_INVALID_SYSTEM_SERVICE;
    }
}

#ifndef JASOS_HOST
static inline void wrmsr(u32 msr, u64 v)
{
    u32 lo = (u32)v, hi = (u32)(v >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

extern void syscall_entry(void);

void syscall_init(void)
{
    u64 efer = rdmsr(0xC0000080);
    wrmsr(0xC0000080, efer | 1); /* SCE */
    /* STAR: kernel CS 0x08 in 47:32, user base 0x10 in 63:48
       sysretq CS = 0x20, SS = 0x18 — matches gdt_init */
    wrmsr(0xC0000081, (0x10ULL << 48) | (0x08ULL << 32));
    wrmsr(0xC0000082, (u64)syscall_entry); /* LSTAR */
    wrmsr(0xC0000084, 0x257fdULL);         /* FMASK: IF DF TF AC NT */
    wrmsr(0xC0000102, (u64)ke_pcb());      /* KERNEL_GS_BASE */
    kprintf("syscall: SCE LSTAR=%p STAR kcs=08 ucs=20\n", (void *)syscall_entry);
}
#endif