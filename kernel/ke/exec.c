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
 * and utilities still share the kernel C until they are fully ring-3.
 * /bin/hello is a real ELF: on hardware the primary thread iretq's to
 * 0x400000; on host we cannot enter ring 3 so a stub thread reports
 * the load and exits.
 */

extern int sh_main(int argc, char **argv);
extern int ls_main(int argc, char **argv);
extern int cat_main(int argc, char **argv);
extern int echo_main(int argc, char **argv);
extern int ps_main(int argc, char **argv);
extern int crash_main(int argc, char **argv);

typedef struct builtin {
    const char *path;
    int (*mainfn)(int, char **);
} builtin_t;

static const builtin_t g_builtins[] = {
    { "/bin/sh",    sh_main },
    { "/bin/ls",    ls_main },
    { "/bin/cat",   cat_main },
    { "/bin/echo",  echo_main },
    { "/bin/ps",    ps_main },
    { "/bin/crash", crash_main },
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
    char *av[2];
    av[0] = p ? p->image : "unknown";
    av[1] = NULL;
    int rc = mainfn ? mainfn(1, av) : 1;
    NtTerminateProcess(HANDLE_CURRENT, rc == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL);
}

#ifdef JASOS_HOST
static void elf_host_stub(void *arg)
{
    (void)arg;
    process_t *p = ke_current_process();
    kprintf("exec: ELF %s loaded host-side (no ring 3) entry=%llx vads=%u\n",
            p ? p->image : "?",
            (unsigned long long)(p ? p->user_entry : 0),
            p ? p->aspace.vad_count : 0);
    NtTerminateProcess(HANDLE_CURRENT, STATUS_SUCCESS);
}
#endif

#ifndef JASOS_HOST
static void user_launch(void *arg)
{
    (void)arg;
    process_t *p = ke_current_process();
    if (!p || !p->user_entry) {
        NtTerminateProcess(HANDLE_CURRENT, STATUS_INVALID_IMAGE_FORMAT);
        return;
    }
    virt_t sp = p->user_stack;
    u64 z = 0;
    /* argc = 0, argv terminator, env terminator. crt0 pops argc. */
    sp -= 8;
    vmm_write_aspace(&p->aspace, sp, &z, 8);
    sp -= 8;
    vmm_write_aspace(&p->aspace, sp, &z, 8);
    sp -= 8;
    vmm_write_aspace(&p->aspace, sp, &z, 8);
    enter_user(p->user_entry, sp, 0x202);
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
    p->user_stack = USER_STACK_TOP - 16;
    p->user_mode = true;
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
    if (!out || !image) return STATUS_INVALID_PARAMETER;
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
    seed_stdio(p);

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
        kprintf("exec: loaded ELF %s pid %llu entry=%llx\n",
                image, (unsigned long long)p->pid,
                (unsigned long long)p->user_entry);
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
    if (h != HANDLE_CURRENT) return STATUS_NOT_IMPLEMENTED;
    sched_exit_thread(stcode);
    return stcode;
}

status_t NtDuplicateObject(handle_t src_proc, handle_t src, handle_t dst_proc,
                           handle_t *out, access_t access)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    process_t *sp, *dp;
    handle_table_t *stbl, *dtbl;
    if (src_proc == HANDLE_CURRENT) sp = ke_current_process();
    else {
        object_t *o;
        status_t st = ht_lookup(ke_current_process() ? &ke_current_process()->handles : NULL,
                                src_proc, PROCESS_QUERY_INFORMATION, OBJ_PROCESS, &o);
        if (!NT_SUCCESS(st)) return st;
        sp = (process_t *)o;
        ob_dereference(o);
    }
    if (dst_proc == HANDLE_CURRENT) dp = ke_current_process();
    else {
        object_t *o;
        status_t st = ht_lookup(ke_current_process() ? &ke_current_process()->handles : NULL,
                                dst_proc, PROCESS_QUERY_INFORMATION, OBJ_PROCESS, &o);
        if (!NT_SUCCESS(st)) return st;
        dp = (process_t *)o;
        ob_dereference(o);
    }
    if (!sp || !dp) return STATUS_INVALID_HANDLE;
    stbl = &sp->handles;
    dtbl = &dp->handles;
    return ht_duplicate(stbl, src, dtbl, access, out);
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
