#include <jasos/syscall.h>
#include <jasos/ke.h>
#include <jasos/fs.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>
#include <jasos/mm.h>

static handle_table_t *cur_ht(void)
{
    process_t *p = ke_current_process();
    return p ? &p->handles : NULL;
}

static bool caller_user(void)
{
    process_t *p = ke_current_process();
    return p && p->user_mode;
}

static status_t sys_path(virt_t up, char *k, usize cap)
{
    if (!up) return STATUS_INVALID_PARAMETER;
    if (caller_user()) return copyinstr(k, up, cap);
    strlcpy(k, (const char *)(uintptr_t)up, cap);
    return STATUS_SUCCESS;
}

static status_t sys_put_u64(virt_t up, u64 v)
{
    if (!up) return STATUS_INVALID_PARAMETER;
    if (caller_user()) return copyout(up, &v, sizeof(v));
    *(u64 *)(uintptr_t)up = v;
    return STATUS_SUCCESS;
}

static status_t sys_put_handle(virt_t up, handle_t h)
{
    return sys_put_u64(up, h);
}

static status_t sys_opt_path(virt_t up, char *k, usize cap, const char **outp)
{
    if (!up) {
        k[0] = 0;
        *outp = NULL;
        return STATUS_SUCCESS;
    }
    status_t st = sys_path(up, k, cap);
    if (!NT_SUCCESS(st)) return st;
    *outp = k;
    return STATUS_SUCCESS;
}

static status_t sys_get_u64(virt_t up, u64 *out)
{
    if (!up || !out) return STATUS_INVALID_PARAMETER;
    if (caller_user()) return copyin(out, up, sizeof(*out));
    *out = *(const u64 *)(uintptr_t)up;
    return STATUS_SUCCESS;
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
    status_t st = ht_lookup(cur_ht(), h, FILE_WRITE_DATA, 0, &o);
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
    if (p->user_mode) {
        /* User RIP is never called in kernel. SMEP would #PF it;
           we refuse to even try. Allocate a stack, park the RIP on
           the TCB, trampoline into enter_user. */
        virt_t rip = (virt_t)(uintptr_t)entry;
        if (rip > USER_CANONICAL_TOP)
            return STATUS_ACCESS_VIOLATION;
#ifdef JASOS_HOST
        (void)arg;
        (void)prio;
        return STATUS_NOT_IMPLEMENTED;
#else
        virt_t stack_base = 0;
        status_t st = vmm_alloc_user(p, &stack_base, USER_STACK_SIZE - PAGE_SIZE,
                                     PAGE_READWRITE, MEM_COMMIT);
        if (!NT_SUCCESS(st)) return st;
        st = psp_create_thread(p, "user", user_thread_entry, NULL, prio,
                               CREATE_SUSPENDED, &t);
        if (!NT_SUCCESS(st)) return st;
        t->user_rip = (virt_t)(uintptr_t)entry;
        t->user_rsp = stack_base + (USER_STACK_SIZE - PAGE_SIZE) - 16;
        sched_ready(t);
        return ht_insert(&p->handles, &t->hdr, THREAD_ALL_ACCESS, out);
#endif
    }
    status_t st = psp_create_thread(p, "user", entry, arg, prio, 0, &t);
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

status_t NtProtectVirtualMemory(handle_t proc, virt_t base, u64 size, u32 prot, u32 *old_prot)
{
    process_t *p;
    object_t *held = NULL;
    if (proc == HANDLE_CURRENT) p = ke_current_process();
    else {
        status_t st = ht_lookup(cur_ht(), proc, PROCESS_VM_OPERATION, OBJ_PROCESS, &held);
        if (!NT_SUCCESS(st)) return st;
        p = (process_t *)held;
    }
    if (!p || size == 0) {
        if (held) ob_dereference(held);
        return STATUS_INVALID_PARAMETER;
    }
    status_t st = vmm_protect_user(p, base, size, prot, old_prot);
    if (held) ob_dereference(held);
    return st;
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

status_t NtWaitForMultipleObjects(handle_t *hs, u32 count, bool wait_all, u64 timeout_ticks)
{
    if (!hs || count == 0 || count > WAIT_OBJECTS_MAX)
        return STATUS_INVALID_PARAMETER;
    object_t *objs[WAIT_OBJECTS_MAX];
    dispatcher_t *ds[WAIT_OBJECTS_MAX];
    handle_table_t *t = cur_ht();
    for (u32 i = 0; i < count; i++) {
        status_t st = ht_lookup(t, hs[i], SYNCHRONIZE, 0, &objs[i]);
        if (!NT_SUCCESS(st))
            st = ht_lookup(t, hs[i], 0, 0, &objs[i]);
        if (!NT_SUCCESS(st)) {
            for (u32 j = 0; j < i; j++) ob_dereference(objs[j]);
            return st;
        }
        if (!objs[i]->wait) {
            for (u32 j = 0; j <= i; j++) ob_dereference(objs[j]);
            return STATUS_INVALID_PARAMETER;
        }
        ds[i] = objs[i]->wait;
    }
    status_t st = ke_wait_multiple(ds, count, wait_all, timeout_ticks);
    for (u32 i = 0; i < count; i++) ob_dereference(objs[i]);
    return st;
}

status_t NtQueryVirtualMemory(handle_t proc, virt_t addr, void *buf, u64 n)
{
    if (!buf || n < sizeof(memory_basic_information_t))
        return STATUS_INFO_LENGTH_MISMATCH;
    process_t *p;
    if (proc == HANDLE_CURRENT) p = ke_current_process();
    else {
        object_t *o;
        status_t st = ht_lookup(cur_ht(), proc, PROCESS_QUERY_INFORMATION, OBJ_PROCESS, &o);
        if (!NT_SUCCESS(st)) return st;
        p = (process_t *)o;
        ob_dereference(o);
    }
    if (!p) return STATUS_INVALID_HANDLE;
    memory_basic_information_t inf;
    memset(&inf, 0, sizeof(inf));
    aspace_t *as = &p->aspace;
    for (u32 i = 0; i < as->vad_count; i++) {
        if (addr >= as->vads[i].start && addr < as->vads[i].end) {
            inf.base = as->vads[i].start;
            inf.alloc_base = as->vads[i].start;
            inf.region_size = as->vads[i].end - as->vads[i].start;
            inf.prot = as->vads[i].prot;
            inf.type = as->vads[i].type;
            inf.state = as->vads[i].committed ? MEM_COMMIT : MEM_RESERVE;
            memcpy(buf, &inf, sizeof(inf));
            return STATUS_SUCCESS;
        }
    }
    inf.base = PAGE_ALIGN_DOWN(addr);
    inf.region_size = PAGE_SIZE;
    inf.state = 0;
    memcpy(buf, &inf, sizeof(inf));
    return STATUS_SUCCESS;
}

status_t NtOpenProcessToken(handle_t proc, access_t access, handle_t *out)
{
    if (!out || access == 0) return STATUS_INVALID_PARAMETER;
    process_t *p;
    object_t *held = NULL;
    if (proc == HANDLE_CURRENT) {
        p = ke_current_process();
    } else {
        status_t st = ht_lookup(cur_ht(), proc, PROCESS_QUERY_INFORMATION, OBJ_PROCESS, &held);
        if (!NT_SUCCESS(st)) return st;
        p = (process_t *)held;
    }
    if (!p) {
        if (held) ob_dereference(held);
        return STATUS_INVALID_HANDLE;
    }
    if (!p->token) {
        if (held) ob_dereference(held);
        return STATUS_NO_TOKEN;
    }
    status_t st = ht_insert(cur_ht(), &p->token->hdr, access, out);
    if (held) ob_dereference(held);
    return st;
}

status_t NtQueryInformationToken(handle_t h, void *buf, u64 n)
{
    if (!buf || n < sizeof(token_basic_information_t))
        return STATUS_INFO_LENGTH_MISMATCH;
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, TOKEN_QUERY, OBJ_TOKEN, &o);
    if (!NT_SUCCESS(st)) return st;
    token_object_t *t = (token_object_t *)o;
    token_basic_information_t *inf = buf;
    inf->pid = t->pid;
    inf->integrity = t->integrity;
    ob_dereference(o);
    return STATUS_SUCCESS;
}

status_t NtDuplicateToken(handle_t src, access_t access, handle_t *out)
{
    if (!out || access == 0) return STATUS_INVALID_PARAMETER;
    object_t *o;
    status_t st = ht_lookup(cur_ht(), src, TOKEN_DUPLICATE, OBJ_TOKEN, &o);
    if (!NT_SUCCESS(st)) return st;
    token_object_t *src_tok = (token_object_t *)o;
    object_t *copy = ob_create(ob_type_token(), NULL, NULL);
    if (!copy) {
        ob_dereference(o);
        return STATUS_NO_MEMORY;
    }
    token_object_t *dst = (token_object_t *)copy;
    dst->pid = src_tok->pid;
    dst->integrity = src_tok->integrity;
    st = ht_insert(cur_ht(), copy, access, out);
    ob_dereference(copy);
    ob_dereference(o);
    return st;
}

/* T18 start: drop-only integrity. A token may go 1→0. Raising is
   ACCESS_DENIED. No privileges bitmap — do not claim Se* exists. */
status_t NtSetInformationToken(handle_t h, u32 integrity)
{
    if (integrity > 1) return STATUS_INVALID_PARAMETER;
    object_t *o;
    status_t st = ht_lookup(cur_ht(), h, TOKEN_ADJUST, OBJ_TOKEN, &o);
    if (!NT_SUCCESS(st)) return st;
    token_object_t *t = (token_object_t *)o;
    if (integrity > t->integrity) {
        ob_dereference(o);
        return STATUS_ACCESS_DENIED;
    }
    t->integrity = integrity;
    ob_dereference(o);
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
    case SYS_NtCreateFile: {
        char path[PATH_MAX];
        handle_t h = 0;
        status_t st = sys_path((virt_t)a2, path, sizeof(path));
        if (!NT_SUCCESS(st)) return st;
        st = NtCreateFile(&h, (access_t)a1, path, (u32)a3, (u32)a4);
        if (NT_SUCCESS(st)) {
            status_t st2 = sys_put_handle((virt_t)a0, h);
            if (!NT_SUCCESS(st2)) { NtClose(h); return st2; }
        }
        return st;
    }
    case SYS_NtReadFile: {
        if (!caller_user())
            return NtReadFile((handle_t)a0, (void *)a1, a2, a3, (u64 *)a4);
        if (a2 > COPY_MAX) return STATUS_INVALID_PARAMETER;
        u64 total = 0;
        status_t st = STATUS_SUCCESS;
        u64 pos = a3;
        while (total < a2) {
            u64 chunk = a2 - total;
            if (chunk > SYSCALL_COPY_MAX) chunk = SYSCALL_COPY_MAX;
            void *kbuf = kalloc((usize)chunk);
            if (!kbuf) return STATUS_NO_MEMORY;
            u64 got = 0;
            st = NtReadFile((handle_t)a0, kbuf, chunk, pos, &got);
            if (got) {
                status_t c = copyout((virt_t)a1 + total, kbuf, got);
                if (!NT_SUCCESS(c)) { kfree(kbuf); return c; }
                total += got;
                if (pos != (u64)-1) pos += got;
            }
            kfree(kbuf);
            if (!NT_SUCCESS(st) && st != STATUS_END_OF_FILE) break;
            if (got < chunk) break;
        }
        if (a4) sys_put_u64((virt_t)a4, total);
        if (total && st == STATUS_END_OF_FILE) return STATUS_SUCCESS;
        return st;
    }
    case SYS_NtWriteFile: {
        if (!caller_user())
            return NtWriteFile((handle_t)a0, (const void *)a1, a2, a3, (u64 *)a4);
        if (a2 > COPY_MAX) return STATUS_INVALID_PARAMETER;
        u64 total = 0;
        status_t st = STATUS_SUCCESS;
        u64 pos = a3;
        while (total < a2) {
            u64 chunk = a2 - total;
            if (chunk > SYSCALL_COPY_MAX) chunk = SYSCALL_COPY_MAX;
            void *kbuf = kalloc((usize)chunk);
            if (!kbuf) return STATUS_NO_MEMORY;
            st = copyin(kbuf, (virt_t)a1 + total, chunk);
            u64 put = 0;
            if (NT_SUCCESS(st))
                st = NtWriteFile((handle_t)a0, kbuf, chunk, pos, &put);
            kfree(kbuf);
            total += put;
            if (pos != (u64)-1) pos += put;
            if (!NT_SUCCESS(st) || put < chunk) break;
        }
        if (a4) sys_put_u64((virt_t)a4, total);
        return st;
    }
    case SYS_NtSetCwd: {
        char path[PATH_MAX];
        status_t st = sys_path((virt_t)a0, path, sizeof(path));
        if (!NT_SUCCESS(st)) return st;
        return NtSetCwd(path);
    }
    case SYS_NtGetCwd: {
        char path[PATH_MAX];
        status_t st = NtGetCwd(path, sizeof(path));
        if (!NT_SUCCESS(st)) return st;
        usize n = strlen(path) + 1;
        if (a1 < n) return STATUS_BUFFER_TOO_SMALL;
        if (caller_user()) return copyout((virt_t)a0, path, n);
        strlcpy((char *)(uintptr_t)a0, path, (usize)a1);
        return STATUS_SUCCESS;
    }
    case SYS_NtCreateProcess: {
        char path[PATH_MAX];
        handle_t h = 0;
        status_t st = sys_path((virt_t)a2, path, sizeof(path));
        if (!NT_SUCCESS(st)) return st;
        u32 argc = (u32)a5;
        if (argc > USER_ARGC_MAX) return STATUS_INVALID_PARAMETER;
        if (argc && !a4) return STATUS_INVALID_PARAMETER;
        if (argc == 0) {
            st = NtCreateProcess(&h, (access_t)a1, path, (u32)a3);
        } else {
            char kargv[USER_ARGC_MAX][USER_ARG_LEN];
            const char *ptrs[USER_ARGC_MAX];
            virt_t ups[USER_ARGC_MAX];
            if (caller_user()) {
                st = copyin(ups, (virt_t)a4, (u64)argc * sizeof(virt_t));
                if (!NT_SUCCESS(st)) return st;
                for (u32 i = 0; i < argc; i++) {
                    st = copyinstr(kargv[i], ups[i], USER_ARG_LEN);
                    if (!NT_SUCCESS(st)) return st;
                    ptrs[i] = kargv[i];
                }
            } else {
                const char *const *av = (const char *const *)(uintptr_t)a4;
                for (u32 i = 0; i < argc; i++) {
                    if (!av[i]) kargv[i][0] = 0;
                    else strlcpy(kargv[i], av[i], USER_ARG_LEN);
                    ptrs[i] = kargv[i];
                }
            }
            st = NtCreateProcessEx(&h, (access_t)a1, path, (u32)a3, ptrs, argc);
        }
        if (NT_SUCCESS(st)) {
            status_t st2 = sys_put_handle((virt_t)a0, h);
            if (!NT_SUCCESS(st2)) return st2;
        }
        return st;
    }
    case SYS_NtQueryDirectoryFile: {
        if (caller_user()) {
            if (a2 == 0 || a2 > SYSCALL_COPY_MAX) return STATUS_INVALID_PARAMETER;
            void *kbuf = kalloc((usize)a2);
            if (!kbuf) return STATUS_NO_MEMORY;
            memset(kbuf, 0, (size_t)a2);
            status_t st = NtQueryDirectoryFile((handle_t)a0, kbuf, a2, a3 != 0);
            usize n = strlen((char *)kbuf);
            if (n + 1 > a2) n = a2;
            else n += 1;
            if (NT_SUCCESS(st) || st == STATUS_END_OF_FILE)
                copyout((virt_t)a1, kbuf, n);
            kfree(kbuf);
            return st;
        }
        return NtQueryDirectoryFile((handle_t)a0, (void *)a1, a2, a3 != 0);
    }
    case SYS_NtTerminateProcess:   return NtTerminateProcess((handle_t)a0, (status_t)a1);
    case SYS_NtYieldExecution:     return NtYieldExecution();
    case SYS_NtDelayExecution:     return NtDelayExecution(a0);
    case SYS_NtQuerySystemInformation: {
        if (caller_user()) {
            if (a2 == 0 || a2 > SYSCALL_COPY_MAX) return STATUS_INVALID_PARAMETER;
            void *kbuf = kalloc((usize)a2);
            if (!kbuf) return STATUS_NO_MEMORY;
            u64 got = 0;
            status_t st = NtQuerySystemInformation((u32)a0, kbuf, a2, &got);
            if (NT_SUCCESS(st) && got)
                copyout((virt_t)a1, kbuf, got);
            if (a3) sys_put_u64((virt_t)a3, got);
            kfree(kbuf);
            return st;
        }
        return NtQuerySystemInformation((u32)a0, (void *)a1, a2, (u64 *)a3);
    }
    case SYS_NtCreateEvent: {
        char name[NAME_MAX];
        const char *nm = NULL;
        handle_t h = 0;
        status_t st = sys_opt_path((virt_t)a1, name, sizeof(name), &nm);
        if (!NT_SUCCESS(st)) return st;
        st = NtCreateEvent(&h, nm, a2 != 0, a3 != 0);
        if (NT_SUCCESS(st)) sys_put_handle((virt_t)a0, h);
        return st;
    }
    case SYS_NtSetEvent:           return NtSetEvent((handle_t)a0);
    case SYS_NtResetEvent:         return NtResetEvent((handle_t)a0);
    case SYS_NtCreateMutex: {
        char name[NAME_MAX];
        const char *nm = NULL;
        handle_t h = 0;
        status_t st = sys_opt_path((virt_t)a1, name, sizeof(name), &nm);
        if (!NT_SUCCESS(st)) return st;
        st = NtCreateMutex(&h, nm, a2 != 0);
        if (NT_SUCCESS(st)) sys_put_handle((virt_t)a0, h);
        return st;
    }
    case SYS_NtReleaseMutex:       return NtReleaseMutex((handle_t)a0);
    case SYS_NtWaitForSingleObject:return NtWaitForSingleObject((handle_t)a0, a1);
    case SYS_NtRaiseException:     return NtRaiseException((status_t)a0);
    case SYS_NtCreateThread: {
        handle_t h = 0;
        status_t st = NtCreateThread(&h, (void (*)(void *))a1, (void *)a2, (u32)a3);
        if (NT_SUCCESS(st)) sys_put_handle((virt_t)a0, h);
        return st;
    }
    case SYS_NtAllocateVirtualMemory: {
        virt_t base = 0;
        status_t st = sys_get_u64((virt_t)a1, &base);
        if (!NT_SUCCESS(st)) return st;
        st = NtAllocateVirtualMemory((handle_t)a0, &base, a2, (u32)a3);
        if (NT_SUCCESS(st)) sys_put_u64((virt_t)a1, base);
        return st;
    }
    case SYS_NtQueryInformationProcess: {
        if (caller_user()) {
            if (a2 < sizeof(sys_process_info_t) || a2 > SYSCALL_COPY_MAX)
                return STATUS_INFO_LENGTH_MISMATCH;
            sys_process_info_t inf;
            memset(&inf, 0, sizeof(inf));
            status_t st = NtQueryInformationProcess((handle_t)a0, &inf, sizeof(inf));
            if (NT_SUCCESS(st)) copyout((virt_t)a1, &inf, sizeof(inf));
            return st;
        }
        return NtQueryInformationProcess((handle_t)a0, (void *)a1, a2);
    }
    case SYS_NtCreatePipe: {
        handle_t r = 0, w = 0;
        status_t st = NtCreatePipe(&r, &w);
        if (NT_SUCCESS(st)) {
            sys_put_handle((virt_t)a0, r);
            sys_put_handle((virt_t)a1, w);
        }
        return st;
    }
    case SYS_NtTerminateThread:    return NtTerminateThread((handle_t)a0, (status_t)a1);
    case SYS_NtDuplicateObject: {
        handle_t h = 0;
        status_t st = NtDuplicateObject((handle_t)a0, (handle_t)a1, (handle_t)a2, &h, (access_t)a4, (u32)a5);
        if (NT_SUCCESS(st)) sys_put_handle((virt_t)a3, h);
        return st;
    }
    case SYS_NtQueryObject: {
        if (caller_user()) {
            object_basic_information_t inf;
            memset(&inf, 0, sizeof(inf));
            status_t st = NtQueryObject((handle_t)a0, &inf, sizeof(inf));
            if (NT_SUCCESS(st)) copyout((virt_t)a1, &inf, sizeof(inf));
            return st;
        }
        return NtQueryObject((handle_t)a0, (void *)a1, a2);
    }
    case SYS_NtCreateSection: {
        handle_t h = 0;
        status_t st = NtCreateSection(&h, (access_t)a1, a2, (u32)a3);
        if (NT_SUCCESS(st)) sys_put_handle((virt_t)a0, h);
        return st;
    }
    case SYS_NtMapViewOfSection: {
        virt_t base = 0;
        status_t st = sys_get_u64((virt_t)a2, &base);
        if (!NT_SUCCESS(st)) return st;
        st = NtMapViewOfSection((handle_t)a0, (handle_t)a1, &base, a3, (u32)a4);
        if (NT_SUCCESS(st)) sys_put_u64((virt_t)a2, base);
        return st;
    }
    case SYS_NtUnmapViewOfSection: return NtUnmapViewOfSection((handle_t)a0, (virt_t)a1);
    case SYS_NtFreeVirtualMemory:  return NtFreeVirtualMemory((handle_t)a0, (virt_t)a1, a2);
    case SYS_NtCreateDirectoryObject: {
        char name[NAME_MAX];
        const char *nm = NULL;
        handle_t h = 0;
        status_t st = sys_opt_path((virt_t)a1, name, sizeof(name), &nm);
        if (!NT_SUCCESS(st)) return st;
        st = NtCreateDirectoryObject(&h, nm);
        if (NT_SUCCESS(st)) sys_put_handle((virt_t)a0, h);
        return st;
    }
    case SYS_NtOpenDirectoryObject: {
        char path[PATH_MAX];
        handle_t h = 0;
        status_t st = sys_path((virt_t)a1, path, sizeof(path));
        if (!NT_SUCCESS(st)) return st;
        st = NtOpenDirectoryObject(&h, path);
        if (NT_SUCCESS(st)) sys_put_handle((virt_t)a0, h);
        return st;
    }
    case SYS_NtCreateTimer: {
        char name[NAME_MAX];
        const char *nm = NULL;
        handle_t h = 0;
        status_t st = sys_opt_path((virt_t)a1, name, sizeof(name), &nm);
        if (!NT_SUCCESS(st)) return st;
        st = NtCreateTimer(&h, nm, a2 != 0);
        if (NT_SUCCESS(st)) sys_put_handle((virt_t)a0, h);
        return st;
    }
    case SYS_NtSetTimer:           return NtSetTimer((handle_t)a0, a1, a2);
    case SYS_NtCancelTimer:        return NtCancelTimer((handle_t)a0);
    case SYS_NtWaitForMultipleObjects: {
        if (a1 == 0 || a1 > WAIT_OBJECTS_MAX) return STATUS_INVALID_PARAMETER;
        handle_t hs[WAIT_OBJECTS_MAX];
        if (caller_user()) {
            status_t st = copyin(hs, (virt_t)a0, a1 * sizeof(handle_t));
            if (!NT_SUCCESS(st)) return st;
        } else {
            memcpy(hs, (void *)(uintptr_t)a0, (size_t)(a1 * sizeof(handle_t)));
        }
        return NtWaitForMultipleObjects(hs, (u32)a1, a2 != 0, a3);
    }
    case SYS_NtQueryVirtualMemory: {
        memory_basic_information_t inf;
        memset(&inf, 0, sizeof(inf));
        status_t st = NtQueryVirtualMemory((handle_t)a0, (virt_t)a1, &inf, sizeof(inf));
        if (!NT_SUCCESS(st)) return st;
        if (caller_user()) return copyout((virt_t)a2, &inf, sizeof(inf));
        if (a3 < sizeof(inf)) return STATUS_INFO_LENGTH_MISMATCH;
        memcpy((void *)(uintptr_t)a2, &inf, sizeof(inf));
        return STATUS_SUCCESS;
    }
    case SYS_NtProtectVirtualMemory: {
        u32 old = 0;
        status_t st = NtProtectVirtualMemory((handle_t)a0, (virt_t)a1, a2, (u32)a3, &old);
        if (NT_SUCCESS(st) && a4) {
            if (caller_user()) copyout((virt_t)a4, &old, sizeof(old));
            else *(u32 *)(uintptr_t)a4 = old;
        }
        return st;
    }
    case SYS_NtOpenProcessToken: {
        handle_t h = 0;
        status_t st = NtOpenProcessToken((handle_t)a0, (access_t)a1, &h);
        if (NT_SUCCESS(st)) {
            status_t st2 = sys_put_handle((virt_t)a2, h);
            if (!NT_SUCCESS(st2)) { NtClose(h); return st2; }
        }
        return st;
    }
    case SYS_NtQueryInformationToken: {
        if (caller_user()) {
            if (a2 < sizeof(token_basic_information_t) || a2 > SYSCALL_COPY_MAX)
                return STATUS_INFO_LENGTH_MISMATCH;
            token_basic_information_t inf;
            memset(&inf, 0, sizeof(inf));
            status_t st = NtQueryInformationToken((handle_t)a0, &inf, sizeof(inf));
            if (NT_SUCCESS(st)) copyout((virt_t)a1, &inf, sizeof(inf));
            return st;
        }
        return NtQueryInformationToken((handle_t)a0, (void *)a1, a2);
    }
    case SYS_NtDuplicateToken: {
        handle_t h = 0;
        status_t st = NtDuplicateToken((handle_t)a0, (access_t)a1, &h);
        if (NT_SUCCESS(st)) {
            status_t st2 = sys_put_handle((virt_t)a2, h);
            if (!NT_SUCCESS(st2)) { NtClose(h); return st2; }
        }
        return st;
    }
    case SYS_NtSetInformationToken:
        return NtSetInformationToken((handle_t)a0, (u32)a1);
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