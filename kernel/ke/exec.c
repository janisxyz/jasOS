#include <jasos/ke.h>
#include <jasos/fs.h>
#include <jasos/elf.h>
#include <jasos/syscall.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>

/*
 * Image spawn. A path is either a known kernel-linked builtin (/bin/sh
 * and friends) or an ELF64 ET_EXEC. Builtins exist because the shell
 * still shares the kernel C until it is fully ring-3.
 * /bin/hello, /bin/echo, /bin/ls, /bin/cat, /bin/ps, /bin/crash are real ELF.
 */

extern int sh_main(int argc, char **argv);

typedef struct builtin {
    const char *path;
    int (*mainfn)(int, char **);
} builtin_t;

static const builtin_t g_builtins[] = {
    { "/bin/sh",    sh_main },
};

int builtin_lookup(const char *path, int (**mainfn)(int, char **))
{
    if (!path) return 0;
    for (u32 i = 0; i < COUNT_OF(g_builtins); i++) {
        if (strcmp(path, g_builtins[i].path) == 0) {
            if (mainfn) *mainfn = g_builtins[i].mainfn;
            return 1;
        }
    }
    return 0;
}

static void builtin_entry(void *arg)
{
    int (*mainfn)(int, char **) = (int (*)(int, char **))arg;
    process_t *p = ke_current_process();
    char *av[USER_ARGC_MAX + 1];
    u32 n = p ? p->argc : 0;
    if (n > USER_ARGC_MAX) n = USER_ARGC_MAX;
    if (n == 0) {
        av[0] = p ? p->image : "unknown";
        av[1] = NULL;
        n = 1;
    } else {
        for (u32 i = 0; i < n; i++)
            av[i] = p->argv[i];
        av[n] = NULL;
    }
    int rc = mainfn ? mainfn((int)n, av) : 1;
    NtTerminateProcess(HANDLE_CURRENT, rc == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL);
}

#ifdef JASOS_HOST
static void elf_host_stub(void *arg)
{
    (void)arg;
    process_t *p = ke_current_process();
    kprintf("exec: ELF %s argc=%u", p ? p->image : "?", p ? p->argc : 0);
    if (p) {
        for (u32 i = 0; i < p->argc && i < USER_ARGC_MAX; i++)
            kprintf(" [%s]", p->argv[i]);
    }
    kprintf(" entry=%llx vads=%u\n",
            (unsigned long long)(p ? p->user_entry : 0),
            p ? p->aspace.vad_count : 0);
    NtTerminateProcess(HANDLE_CURRENT, STATUS_SUCCESS);
}
#endif

static status_t psp_copy_argv(process_t *p, const char *const *argv, u32 argc)
{
    if (!p) return STATUS_INVALID_PARAMETER;
    if (argc > USER_ARGC_MAX) return STATUS_INVALID_PARAMETER;
    if (argc && !argv) return STATUS_INVALID_PARAMETER;
    if (argc == 0) {
        p->argc = 1;
        strlcpy(p->argv[0], p->image[0] ? p->image : "unknown", USER_ARG_LEN);
        return STATUS_SUCCESS;
    }
    p->argc = argc;
    for (u32 i = 0; i < argc; i++) {
        if (!argv[i])
            p->argv[i][0] = 0;
        else
            strlcpy(p->argv[i], argv[i], USER_ARG_LEN);
    }
    return STATUS_SUCCESS;
}

/*
 * T16: argc/argv on the user stack. Strings first (high VA), then
 * env NULL, argv NULL, argv[n-1]..argv[0], argc (low VA). crt0 pops
 * argc into rdi and takes rsi = rsp as argv.
 *
 * Failure: a write miss means the stack VAD is gone; return 0.
 */
static virt_t psp_write_initial_stack(process_t *p)
{
    if (!p || !p->user_mode) return 0;
    u32 n = p->argc;
    if (n == 0) {
        p->argc = 1;
        strlcpy(p->argv[0], p->image, USER_ARG_LEN);
        n = 1;
    }
    if (n > USER_ARGC_MAX) n = USER_ARGC_MAX;
    virt_t sp = USER_STACK_TOP - 16;
    virt_t str_va[USER_ARGC_MAX];
    for (u32 i = 0; i < n; i++) {
        u64 len = 0;
        while (p->argv[i][len]) len++;
        len++;
        sp -= (len + 7u) & ~7u;
        if (!NT_SUCCESS(vmm_write_aspace(&p->aspace, sp, p->argv[i], len)))
            return 0;
        str_va[i] = sp;
    }
    u64 z = 0;
    sp -= 8;
    if (!NT_SUCCESS(vmm_write_aspace(&p->aspace, sp, &z, 8))) return 0;
    sp -= 8;
    if (!NT_SUCCESS(vmm_write_aspace(&p->aspace, sp, &z, 8))) return 0;
    for (u32 i = n; i-- > 0; ) {
        sp -= 8;
        if (!NT_SUCCESS(vmm_write_aspace(&p->aspace, sp, &str_va[i], 8)))
            return 0;
    }
    u64 argc64 = n;
    sp -= 8;
    if (!NT_SUCCESS(vmm_write_aspace(&p->aspace, sp, &argc64, 8))) return 0;
    p->user_stack = sp;
    return sp;
}

#ifndef JASOS_HOST
static void user_launch(void *arg)
{
    (void)arg;
    process_t *p = ke_current_process();
    if (!p || !p->user_entry || !p->user_stack) {
        NtTerminateProcess(HANDLE_CURRENT, STATUS_INVALID_IMAGE_FORMAT);
        return;
    }
    enter_user(p->user_entry, p->user_stack, 0x202);
}

void user_thread_entry(void *arg)
{
    (void)arg;
    thread_t *t = ke_current();
    if (!t || !t->user_rip)
        NtTerminateProcess(HANDLE_CURRENT, STATUS_INVALID_PARAMETER);
    enter_user(t->user_rip, t->user_rsp, 0x202);
}
#else
void user_thread_entry(void *arg)
{
    (void)arg;
    NtTerminateProcess(HANDLE_CURRENT, STATUS_NOT_IMPLEMENTED);
}
#endif

status_t psp_exec_image(process_t *p, const u8 *image, u64 len, virt_t *entry_out)
{
    virt_t entry = 0;
    status_t st = elf_load(p, image, len, &entry);
    if (!NT_SUCCESS(st)) return st;
    virt_t stack_base = USER_STACK_TOP - USER_STACK_SIZE + PAGE_SIZE;
    st = vmm_alloc_user(p, &stack_base, USER_STACK_SIZE - PAGE_SIZE, PAGE_READWRITE, MEM_COMMIT);
    if (!NT_SUCCESS(st)) return st;
    p->user_entry = entry;
    p->user_mode = true;
    if (!psp_write_initial_stack(p))
        return STATUS_NO_MEMORY;
    if (entry_out) *entry_out = entry;
    return STATUS_SUCCESS;
}

static status_t seed_stdio(process_t *p)
{
    file_object_t *in, *out, *err;
    status_t st = vfs_open("/dev/console", FILE_READ_DATA | FILE_WRITE_DATA,
                           FILE_OPEN, FILE_NON_DIRECTORY_FILE, &in);
    if (!NT_SUCCESS(st)) return STATUS_SUCCESS; /* console optional */
    vfs_open("/dev/console", FILE_READ_DATA | FILE_WRITE_DATA,
             FILE_OPEN, FILE_NON_DIRECTORY_FILE, &out);
    vfs_open("/dev/console", FILE_READ_DATA | FILE_WRITE_DATA,
             FILE_OPEN, FILE_NON_DIRECTORY_FILE, &err);
    ht_insert(&p->handles, &in->hdr, FILE_READ_DATA | FILE_WRITE_DATA, &p->std_in);
    ht_insert(&p->handles, &out->hdr, FILE_READ_DATA | FILE_WRITE_DATA, &p->std_out);
    ht_insert(&p->handles, &err->hdr, FILE_READ_DATA | FILE_WRITE_DATA, &p->std_err);
    ob_dereference(&in->hdr);
    ob_dereference(&out->hdr);
    ob_dereference(&err->hdr);
    return STATUS_SUCCESS;
}

status_t NtCreateProcess(handle_t *out, access_t access, const char *image, u32 flags)
{
    return NtCreateProcessEx(out, access, image, flags, NULL, 0);
}

status_t NtCreateProcessEx(handle_t *out, access_t access, const char *image, u32 flags,
                           const char *const *argv, u32 argc)
{
    if (!out || !image) return STATUS_INVALID_PARAMETER;
    if (argc > USER_ARGC_MAX) return STATUS_INVALID_PARAMETER;
    if (argc && !argv) return STATUS_INVALID_PARAMETER;
    process_t *parent = ke_current_process();
    if (!parent) parent = psp_system_process();

    u8 *bytes = NULL;
    u64 len = 0;
    status_t st = vfs_read_all(image, &bytes, &len);
    if (!NT_SUCCESS(st)) return st;

    process_t *p;
    st = psp_create_process(image, parent, &p);
    if (!NT_SUCCESS(st)) {
        kfree(bytes);
        return st;
    }
    st = psp_copy_argv(p, argv, argc);
    if (!NT_SUCCESS(st)) {
        kfree(bytes);
        ob_dereference(&p->hdr);
        return st;
    }
    seed_stdio(p);
    if (parent)
        ht_inherit_table(&parent->handles, &p->handles);

    int (*mainfn)(int, char **) = NULL;
    int is_builtin = builtin_lookup(image, &mainfn);
    int is_elf = (len >= 4 && bytes[0] == 0x7f && bytes[1] == 'E' &&
                  bytes[2] == 'L' && bytes[3] == 'F');

    if (is_elf) {
        st = psp_exec_image(p, bytes, len, NULL);
        if (!NT_SUCCESS(st)) {
            kfree(bytes);
            ob_dereference(&p->hdr);
            return st;
        }
        kprintf("exec: loaded ELF %s pid %llu entry=%llx argc=%u\n",
                image, (unsigned long long)p->pid,
                (unsigned long long)p->user_entry, p->argc);
    } else if (!is_builtin && !(flags & CREATE_NO_IMAGE)) {
        kfree(bytes);
        ob_dereference(&p->hdr);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    kfree(bytes);
    p->builtin = mainfn;

    if (!(flags & CREATE_SUSPENDED)) {
        thread_t *t = NULL;
        if (is_builtin) {
            st = psp_create_thread(p, p->image, builtin_entry, (void *)mainfn,
                                   PRIORITY_NORMAL, 0, &t);
        } else if (is_elf) {
#ifdef JASOS_HOST
            st = psp_create_thread(p, p->image, elf_host_stub, NULL,
                                   PRIORITY_NORMAL, 0, &t);
#else
            st = psp_create_thread(p, p->image, user_launch, NULL,
                                   PRIORITY_NORMAL, 0, &t);
#endif
        }
        (void)t;
        if (!NT_SUCCESS(st)) {
            ob_dereference(&p->hdr);
            return st;
        }
    }

    st = ht_insert(&parent->handles, &p->hdr,
                   access ? access : PROCESS_ALL_ACCESS, out);
    ob_dereference(&p->hdr);
    return st;
}

status_t NtTerminateThread(handle_t h, status_t stcode)
{
    if (h == HANDLE_CURRENT) {
        sched_exit_thread(stcode);
        return stcode;
    }
    object_t *o;
    status_t st = ht_lookup(ke_current_process() ? &ke_current_process()->handles : NULL,
                            h, THREAD_TERMINATE, OBJ_THREAD, &o);
    if (!NT_SUCCESS(st)) return st;
    thread_t *t = (thread_t *)o;
    sched_kill_thread(t, stcode);
    ob_dereference(o);
    return STATUS_SUCCESS;
}

status_t NtDuplicateObject(handle_t src_proc, handle_t src, handle_t dst_proc,
                           handle_t *out, access_t access, u32 flags)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    process_t *sp = NULL, *dp = NULL;
    object_t *so = NULL, *dobj = NULL;
    if (src_proc == HANDLE_CURRENT) sp = ke_current_process();
    else {
        status_t st = ht_lookup(ke_current_process() ? &ke_current_process()->handles : NULL,
                                src_proc, PROCESS_DUP_HANDLE, OBJ_PROCESS, &so);
        if (!NT_SUCCESS(st)) return st;
        sp = (process_t *)so;
    }
    if (dst_proc == HANDLE_CURRENT) dp = ke_current_process();
    else {
        status_t st = ht_lookup(ke_current_process() ? &ke_current_process()->handles : NULL,
                                dst_proc, PROCESS_DUP_HANDLE, OBJ_PROCESS, &dobj);
        if (!NT_SUCCESS(st)) {
            if (so) ob_dereference(so);
            return st;
        }
        dp = (process_t *)dobj;
    }
    if (!sp || !dp) {
        if (so) ob_dereference(so);
        if (dobj) ob_dereference(dobj);
        return STATUS_INVALID_HANDLE;
    }
    if (flags & DUPLICATE_SAME_ACCESS)
        access = 0;
    status_t st = ht_duplicate(&sp->handles, src, &dp->handles, access, out);
    if (NT_SUCCESS(st) && (flags & DUPLICATE_INHERIT))
        ht_set_inherit(&dp->handles, *out, true);
    if (NT_SUCCESS(st) && (flags & DUPLICATE_CLOSE_SOURCE))
        ht_close(&sp->handles, src);
    if (so) ob_dereference(so);
    if (dobj) ob_dereference(dobj);
    return st;
}

status_t NtQueryObject(handle_t h, void *buf, u64 n)
{
    if (!buf || n < sizeof(object_basic_information_t))
        return STATUS_INFO_LENGTH_MISMATCH;
    object_t *o;
    status_t st = ht_lookup(ke_current_process() ? &ke_current_process()->handles : NULL,
                            h, 0, 0, &o);
    if (!NT_SUCCESS(st)) return st;
    object_basic_information_t *inf = buf;
    memset(inf, 0, sizeof(*inf));
    inf->kind = o->type ? o->type->kind : 0;
    inf->pointer_count = o->pointer_count;
    inf->handle_count = o->handle_count;
    if (o->type && o->type->name) strlcpy(inf->type_name, o->type->name, sizeof(inf->type_name));
    strlcpy(inf->name, o->name, sizeof(inf->name));
    ob_dereference(o);
    return STATUS_SUCCESS;
}

status_t NtCreateDirectoryObject(handle_t *out, const char *name)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    object_t *o = ob_create(ob_type_directory(), name, name ? ob_dir_bno() : NULL);
    if (!o) return STATUS_NO_MEMORY;
    directory_object_t *d = (directory_object_t *)o;
    spin_init(&d->lock, "dir", LOCK_RANK_OB);
    for (int i = 0; i < 37; i++) list_init(&d->buckets[i]);
    d->count = 0;
    status_t st = ht_insert(ke_current_process() ? &ke_current_process()->handles : NULL,
                            o, DIRECTORY_QUERY | DIRECTORY_TRAVERSE | DIRECTORY_CREATE_OBJECT, out);
    ob_dereference(o);
    return st;
}

status_t NtOpenDirectoryObject(handle_t *out, const char *path)
{
    if (!out || !path) return STATUS_INVALID_PARAMETER;
    object_t *o;
    status_t st = ob_lookup(path, OBJ_DIRECTORY, &o);
    if (!NT_SUCCESS(st)) return st;
    st = ht_insert(ke_current_process() ? &ke_current_process()->handles : NULL,
                   o, DIRECTORY_QUERY | DIRECTORY_TRAVERSE, out);
    ob_dereference(o);
    return st;
}

status_t NtFreeVirtualMemory(handle_t proc, virt_t base, u64 size)
{
    process_t *p;
    if (proc == HANDLE_CURRENT) p = ke_current_process();
    else {
        object_t *o;
        status_t st = ht_lookup(ke_current_process() ? &ke_current_process()->handles : NULL,
                                proc, PROCESS_VM_OPERATION, OBJ_PROCESS, &o);
        if (!NT_SUCCESS(st)) return st;
        p = (process_t *)o;
        ob_dereference(o);
    }
    if (!p) return STATUS_INVALID_HANDLE;
    return vmm_free_user(p, base, size);
}
