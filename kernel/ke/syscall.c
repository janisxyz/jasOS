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
    status_t st = ht_lookup(cur_ht(), h, FILE_READ_DATA, OBJ_FILE, &o);
    if (!NT_SUCCESS(st)) return st;
    file_object_t *f = (file_object_t *)o;
    if (off != (u64)-1) f->offset = off;
    st = vfs_read(f, buf, n, got);
    ob_dereference(o);
    return st;
}

status_t NtWriteFile(handle_t h, const void *buf, u64 n, u64 off, u64 *put)
{
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, 0, OBJ_FILE, &o);
    if (!NT_SUCCESS(st)) return st;
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
    event_object_t *e;
    status_t st = ob_create_event(NULL, true, false, &e);
    if (!NT_SUCCESS(st)) return st;
    /* v1: yield `ticks` times. A real timer object comes next. */
    for (u64 i = 0; i < ticks; i++) {
        ke_on_tick();
        sched_yield();
    }
    ob_dereference(&e->hdr);
    return STATUS_SUCCESS;
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
    case SYS_NtCreateMutex:        return NtCreateMutex((handle_t *)a0, (const char *)a1, a2 != 0);
    case SYS_NtReleaseMutex:       return NtReleaseMutex((handle_t)a0);
    case SYS_NtWaitForSingleObject:return NtWaitForSingleObject((handle_t)a0, a1);
    case SYS_NtRaiseException:     return NtRaiseException((status_t)a0);
    default:                       return STATUS_INVALID_SYSTEM_SERVICE;
    }
}

#ifndef JASOS_HOST
void syscall_init(void)
{
    /* IA32_EFER, STAR, LSTAR, FMASK — filled in ke/syscall_entry.S world.
       v1 kernel-linked userland does not yet hit syscall; the gate is
       assembled and the MSRs are written so gdb can see them. */
    kprintf("syscall: gate armed (kernel-linked userland in v1)\n");
}
#endif
