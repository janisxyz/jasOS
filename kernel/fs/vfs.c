#include <jasos/fs.h>
#include <jasos/mm.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/ke.h>
#include <jasos/elf.h>
#include <jasos/config.h>
#include <jasos/io.h>

#if defined(__has_include)
#  if __has_include("hello_blob.h")
#    include "hello_blob.h"
#    define HAVE_HELLO_BLOB 1
#  endif
#  if __has_include("echo_blob.h")
#    include "echo_blob.h"
#    define HAVE_ECHO_BLOB 1
#  endif
#  if __has_include("ls_blob.h")
#    include "ls_blob.h"
#    define HAVE_LS_BLOB 1
#  endif
#  if __has_include("cat_blob.h")
#    include "cat_blob.h"
#    define HAVE_CAT_BLOB 1
#  endif
#  if __has_include("ps_blob.h")
#    include "ps_blob.h"
#    define HAVE_PS_BLOB 1
#  endif
#  if __has_include("crash_blob.h")
#    include "crash_blob.h"
#    define HAVE_CRASH_BLOB 1
#  endif
#endif

static vnode_t    *g_root_vnode;
static spinlock_t  g_vfs_lock = SPINLOCK_INIT("vfs", LOCK_RANK_VFS);

vnode_t *vfs_root(void) { return g_root_vnode; }

static vnode_t *vn_alloc(const char *name, u32 kind)
{
    vnode_t *v = kalloc_zero(sizeof(*v));
    if (!v) return NULL;
    strlcpy(v->name, name, NAME_MAX);
    v->kind = kind;
    v->mode = (kind == VNODE_DIR) ? 0755 : 0644;
    v->ref = 1;
    list_init(&v->children);
    list_init(&v->sibling);
    spin_init(&v->lock, "vnode", LOCK_RANK_VFS);
    return v;
}

static void vn_attach(vnode_t *parent, vnode_t *child)
{
    child->parent = parent;
    list_insert_tail(&parent->children, &child->sibling);
}

static vnode_t *vn_child(vnode_t *dir, const char *name)
{
    for (list_t *e = dir->children.next; e != &dir->children; e = e->next) {
        vnode_t *c = CONTAINER_OF(e, vnode_t, sibling);
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

status_t path_norm(const char *cwd, const char *in, char *out, usize cap)
{
    char tmp[PATH_MAX];
    if (!in || !out || cap < 2) return STATUS_INVALID_PARAMETER;
    if (in[0] == '/') strlcpy(tmp, in, PATH_MAX);
    else {
        strlcpy(tmp, cwd ? cwd : "/", PATH_MAX);
        if (tmp[strlen(tmp) - 1] != '/') strlcat(tmp, "/", PATH_MAX);
        strlcat(tmp, in, PATH_MAX);
    }
    char *parts[PATH_DEPTH_MAX];
    u32 n = 0;
    char buf[PATH_MAX];
    strlcpy(buf, tmp, PATH_MAX);
    char *s = buf;
    if (*s == '/') s++;
    while (*s) {
        char *start = s;
        while (*s && *s != '/') s++;
        if (*s) *s++ = 0;
        if (start[0] == 0 || (start[0] == '.' && start[1] == 0)) continue;
        if (start[0] == '.' && start[1] == '.' && start[2] == 0) {
            if (n) n--;
            continue;
        }
        if (n >= PATH_DEPTH_MAX) return STATUS_NAME_TOO_LONG;
        parts[n++] = start;
    }
    out[0] = '/';
    out[1] = 0;
    for (u32 i = 0; i < n; i++) {
        if (i) strlcat(out, "/", cap);
        strlcat(out, parts[i], cap);
    }
    if (n == 0) { out[0] = '/'; out[1] = 0; }
    return STATUS_SUCCESS;
}

static status_t walk(const char *abs, vnode_t **parent_out, vnode_t **leaf, char *leafname, bool last_must_exist)
{
    if (abs[0] != '/') return STATUS_OBJECT_NAME_INVALID;
    vnode_t *cur = g_root_vnode;
    const char *p = abs + 1;
    if (*p == 0) {
        if (parent_out) *parent_out = cur;
        if (leaf) *leaf = cur;
        if (leafname) leafname[0] = 0;
        return STATUS_SUCCESS;
    }
    char comp[NAME_MAX];
    vnode_t *parent = cur;
    for (;;) {
        u32 i = 0;
        while (*p && *p != '/' && i + 1 < NAME_MAX) comp[i++] = *p++;
        comp[i] = 0;
        int more = (*p == '/');
        if (*p == '/') p++;
        parent = cur;
        vnode_t *next = vn_child(cur, comp);
        if (!*p && !more) {
            if (leafname) strlcpy(leafname, comp, NAME_MAX);
            if (parent_out) *parent_out = parent;
            if (!next && last_must_exist) return STATUS_NO_SUCH_FILE;
            if (leaf) *leaf = next;
            return STATUS_SUCCESS;
        }
        if (!next || next->kind != VNODE_DIR) return STATUS_OBJECT_PATH_NOT_FOUND;
        cur = next;
        while (*p == '/') p++;
        if (*p == 0) {
            if (leafname) strlcpy(leafname, "", NAME_MAX);
            if (parent_out) *parent_out = parent;
            if (leaf) *leaf = cur;
            return STATUS_SUCCESS;
        }
    }
}

void ramfs_init(vnode_t **root_out)
{
    vnode_t *r = vn_alloc("", VNODE_DIR);
    if (!r) panic("ramfs root");
    r->name[0] = 0;
    *root_out = r;
}

void vfs_init(void)
{
    ramfs_init(&g_root_vnode);
    kprintf("vfs: ramfs mounted on /\n");
}

status_t vfs_mkdir(const char *path)
{
    char abs[PATH_MAX], leaf[NAME_MAX];
    process_t *p = ke_current_process();
    status_t st = path_norm(p ? p->cwd : "/", path, abs, PATH_MAX);
    if (!NT_SUCCESS(st)) return st;
    vnode_t *parent, *exist;
    st = walk(abs, &parent, &exist, leaf, false);
    if (!NT_SUCCESS(st)) return st;
    if (exist) return STATUS_OBJECT_NAME_COLLISION;
    if (!leaf[0]) return STATUS_INVALID_PARAMETER;
    vnode_t *d = vn_alloc(leaf, VNODE_DIR);
    if (!d) return STATUS_NO_MEMORY;
    spin_lock(&g_vfs_lock);
    if (vn_child(parent, leaf)) {
        spin_unlock(&g_vfs_lock);
        kfree(d);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    vn_attach(parent, d);
    spin_unlock(&g_vfs_lock);
    return STATUS_SUCCESS;
}

static file_object_t *fo_from_vnode(vnode_t *v, access_t access)
{
    file_object_t *f = (file_object_t *)ob_create(ob_type_file(), v->name, NULL);
    if (!f) return NULL;
    f->vnode = v;
    f->offset = 0;
    f->access = access;
    v->ref++;
    return f;
}

status_t vfs_open(const char *path, access_t access, u32 disp, u32 opts, file_object_t **out)
{
    char abs[PATH_MAX], leaf[NAME_MAX];
    process_t *p = ke_current_process();
    status_t st = path_norm(p ? p->cwd : "/", path, abs, PATH_MAX);
    if (!NT_SUCCESS(st)) return st;
    vnode_t *parent, *v;
    st = walk(abs, &parent, &v, leaf, disp == FILE_OPEN);
    if (!NT_SUCCESS(st) && disp == FILE_OPEN) return st;
    if (v && v->kind == VNODE_DIR && (opts & FILE_NON_DIRECTORY_FILE))
        return STATUS_FILE_IS_A_DIRECTORY;
    if (v && v->kind != VNODE_DIR && (opts & FILE_DIRECTORY_FILE))
        return STATUS_NOT_A_DIRECTORY;
    if (!v) {
        if (disp == FILE_OPEN) return STATUS_NO_SUCH_FILE;
        if (!leaf[0]) return STATUS_INVALID_PARAMETER;
        vnode_t *nv = vn_alloc(leaf, (opts & FILE_DIRECTORY_FILE) ? VNODE_DIR : VNODE_FILE);
        if (!nv) return STATUS_NO_MEMORY;
        spin_lock(&g_vfs_lock);
        vnode_t *race = vn_child(parent, leaf);
        if (race) {
            spin_unlock(&g_vfs_lock);
            kfree(nv);
            v = race;
        } else {
            vn_attach(parent, nv);
            spin_unlock(&g_vfs_lock);
            v = nv;
        }
    } else if (disp == FILE_CREATE) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    file_object_t *f = fo_from_vnode(v, access);
    if (!f) return STATUS_NO_MEMORY;
    *out = f;
    return STATUS_SUCCESS;
}

status_t vfs_create(const char *path, file_object_t **out)
{
    return vfs_open(path, FILE_READ_DATA | FILE_WRITE_DATA, FILE_CREATE, FILE_NON_DIRECTORY_FILE, out);
}

status_t vfs_read(file_object_t *f, void *buf, u64 n, u64 *got)
{
    if (!f || !buf) return STATUS_INVALID_PARAMETER;
    if (!(f->access & FILE_READ_DATA) && !(f->access & GENERIC_READ))
        return STATUS_ACCESS_DENIED;
    vnode_t *v = f->vnode;
    if (v->kind == VNODE_CHAR) {
        u64 nread = 0;
        for (u64 i = 0; i < n; i++) {
            int c = serial_poll_char();
            if (c < 0) break;
            ((char *)buf)[i] = (char)c;
            nread++;
            if (c == '\n') { i++; break; }
        }
        if (got) *got = nread;
        return nread ? STATUS_SUCCESS : STATUS_END_OF_FILE;
    }
    if (v->kind == VNODE_BLOCK) {
        if (!v->device) return STATUS_NO_SUCH_FILE;
        irp_t *irp = io_alloc_irp(IRP_MJ_READ);
        if (!irp) return STATUS_NO_MEMORY;
        irp->buffer = buf;
        irp->length = n;
        irp->offset = f->offset;
        status_t st = io_call_driver(v->device, irp);
        u64 nread = irp->information;
        f->offset += nread;
        if (got) *got = nread;
        io_free_irp(irp);
        return st;
    }
    if (v->kind != VNODE_FILE) return STATUS_INVALID_PARAMETER;
    spin_lock(&v->lock);
    u64 off = f->offset;
    if (off >= v->size) {
        spin_unlock(&v->lock);
        if (got) *got = 0;
        return STATUS_END_OF_FILE;
    }
    u64 nread = MIN(n, v->size - off);
    memcpy(buf, v->data + off, (size_t)nread);
    f->offset += nread;
    spin_unlock(&v->lock);
    if (got) *got = nread;
    return STATUS_SUCCESS;
}

status_t vfs_write(file_object_t *f, const void *buf, u64 n, u64 *put)
{
    if (!f || !buf) return STATUS_INVALID_PARAMETER;
    if (!(f->access & FILE_WRITE_DATA) && !(f->access & FILE_APPEND_DATA) &&
        !(f->access & GENERIC_WRITE))
        return STATUS_ACCESS_DENIED;
    vnode_t *v = f->vnode;
    if (v->kind == VNODE_CHAR) {
        const char *s = buf;
        for (u64 i = 0; i < n; i++) serial_putchar(s[i]);
        if (put) *put = n;
        return STATUS_SUCCESS;
    }
    if (v->kind == VNODE_BLOCK) {
        if (!v->device) return STATUS_NO_SUCH_FILE;
        irp_t *irp = io_alloc_irp(IRP_MJ_WRITE);
        if (!irp) return STATUS_NO_MEMORY;
        irp->buffer = (void *)buf;
        irp->length = n;
        irp->offset = f->offset;
        status_t st = io_call_driver(v->device, irp);
        u64 nput = irp->information;
        f->offset += nput;
        if (put) *put = nput;
        io_free_irp(irp);
        return st;
    }
    if (v->kind != VNODE_FILE) return STATUS_FILE_IS_A_DIRECTORY;
    spin_lock(&v->lock);
    u64 off = (f->access & FILE_APPEND_DATA) ? v->size : f->offset;
    u64 need = off + n;
    u8 *recycle = NULL;
    if (need > v->cap) {
        u64 ncap = MAX(need, v->cap ? v->cap * 2 : 256);
        ncap = PAGE_ALIGN_UP(ncap);
        spin_unlock(&v->lock);
        u8 *nd = kalloc(ncap);
        if (!nd) return STATUS_NO_MEMORY;
        spin_lock(&v->lock);
        off = (f->access & FILE_APPEND_DATA) ? v->size : f->offset;
        need = off + n;
        if (need > v->cap) {
            if (v->data && v->size) memcpy(nd, v->data, (size_t)v->size);
            recycle = v->data;
            v->data = nd;
            v->cap = ncap;
        } else {
            recycle = nd;
        }
    }
    memcpy(v->data + off, buf, (size_t)n);
    if (off + n > v->size) v->size = off + n;
    f->offset = off + n;
    spin_unlock(&v->lock);
    if (recycle) kfree(recycle);
    if (put) *put = n;
    return STATUS_SUCCESS;
}

status_t vfs_readdir(file_object_t *f, char *buf, u64 cap, u64 *put, bool restart)
{
    if (!f || !buf) return STATUS_INVALID_PARAMETER;
    vnode_t *v = f->vnode;
    if (v->kind != VNODE_DIR) return STATUS_NOT_A_DIRECTORY;
    if (restart) f->offset = 0;
    u64 written = 0;
    u64 idx = 0;
    buf[0] = 0;
    for (list_t *e = v->children.next; e != &v->children; e = e->next) {
        vnode_t *c = CONTAINER_OF(e, vnode_t, sibling);
        if (idx++ < f->offset) continue;
        usize need = strlen(c->name) + 2;
        if (written + need >= cap) break;
        if (written) buf[written++] = '\n';
        usize k = strlcpy(buf + written, c->name, cap - written);
        written += k;
        f->offset++;
    }
    if (put) *put = written;
    return written ? STATUS_SUCCESS : STATUS_END_OF_FILE;
}

status_t vfs_stat_path(const char *path, vnode_t **out)
{
    char abs[PATH_MAX], leaf[NAME_MAX];
    process_t *p = ke_current_process();
    status_t st = path_norm(p ? p->cwd : "/", path, abs, PATH_MAX);
    if (!NT_SUCCESS(st)) return st;
    vnode_t *parent, *v;
    st = walk(abs, &parent, &v, leaf, true);
    if (!NT_SUCCESS(st)) return st;
    if (!v) return STATUS_NO_SUCH_FILE;
    if (out) *out = v;
    return STATUS_SUCCESS;
}

status_t vfs_unlink(const char *path)
{
    char abs[PATH_MAX], leaf[NAME_MAX];
    process_t *p = ke_current_process();
    status_t st = path_norm(p ? p->cwd : "/", path, abs, PATH_MAX);
    if (!NT_SUCCESS(st)) return st;
    vnode_t *parent, *v;
    st = walk(abs, &parent, &v, leaf, true);
    if (!NT_SUCCESS(st) || !v) return STATUS_NO_SUCH_FILE;
    if (v->kind == VNODE_DIR && !list_empty(&v->children)) return STATUS_CANNOT_DELETE;
    spin_lock(&g_vfs_lock);
    list_remove(&v->sibling);
    u8 *data = v->data;
    spin_unlock(&g_vfs_lock);
    if (data) kfree(data);
    kfree(v);
    return STATUS_SUCCESS;
}

static status_t seed_file(const char *path, const char *text)
{
    file_object_t *f;
    status_t st = vfs_open(path, FILE_WRITE_DATA, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &f);
    if (!NT_SUCCESS(st)) {
        st = vfs_open(path, FILE_WRITE_DATA, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, &f);
        if (!NT_SUCCESS(st)) return st;
    }
    u64 n = 0;
    st = vfs_write(f, text, strlen(text), &n);
    ob_dereference(&f->hdr);
    return st;
}

status_t vfs_seed_initrd(void)
{
    vfs_mkdir("/bin");
    vfs_mkdir("/etc");
    vfs_mkdir("/tmp");
    vfs_mkdir("/proc");
    vfs_mkdir("/dev");
    vfs_mkdir("/usr");
    vfs_mkdir("/usr/share");
    {
        vnode_t *parent, *v;
        char leaf[NAME_MAX];
        if (NT_SUCCESS(walk("/dev", &parent, &v, leaf, true)) && v) {
            vnode_t *con = vn_alloc("console", VNODE_CHAR);
            if (con) vn_attach(v, con);
            vnode_t *rd = vn_alloc("ram0", VNODE_BLOCK);
            if (rd) {
                rd->device = ramdisk_device();
                rd->size = ramdisk_size();
                vn_attach(v, rd);
            }
        }
    }
    seed_file("/etc/motd",
              "jasOS Aegis " JASOS_VERSION_STR "\n"
              "hybrid kernel — objects, handles, NTSTATUS\n"
              "type help\n");
    seed_file("/etc/hostname", "aegis\n");
    seed_file("/etc/version", JASOS_VERSION_STR "\n");
    seed_file("/usr/share/welcome",
              "Welcome to jasOS.\nThis is an operating system, not a website.\n");
    seed_file("/bin/sh", "BUILTIN\n");
    {
#ifdef HAVE_HELLO_BLOB
        if (hello_elf_blob_len > 64)
            vfs_write_bytes("/bin/hello", hello_elf_blob, hello_elf_blob_len);
        else
#endif
        {
            u8 mini[128];
            u64 n = elf_make_minimal_hello(mini, sizeof(mini));
            if (n) vfs_write_bytes("/bin/hello", mini, n);
        }
    }
    {
#ifdef HAVE_ECHO_BLOB
        if (echo_elf_blob_len > 64)
            vfs_write_bytes("/bin/echo", echo_elf_blob, echo_elf_blob_len);
        else
#endif
        {
            seed_file("/bin/echo", "MISSING_ELF\n");
        }
    }
    {
#ifdef HAVE_LS_BLOB
        if (ls_elf_blob_len > 64)
            vfs_write_bytes("/bin/ls", ls_elf_blob, ls_elf_blob_len);
        else
#endif
        {
            seed_file("/bin/ls", "MISSING_ELF\n");
        }
    }
    {
#ifdef HAVE_CAT_BLOB
        if (cat_elf_blob_len > 64)
            vfs_write_bytes("/bin/cat", cat_elf_blob, cat_elf_blob_len);
        else
#endif
        {
            seed_file("/bin/cat", "MISSING_ELF\n");
        }
    }
    {
#ifdef HAVE_PS_BLOB
        if (ps_elf_blob_len > 64)
            vfs_write_bytes("/bin/ps", ps_elf_blob, ps_elf_blob_len);
        else
#endif
        {
            seed_file("/bin/ps", "MISSING_ELF\n");
        }
    }
    {
#ifdef HAVE_CRASH_BLOB
        if (crash_elf_blob_len > 64)
            vfs_write_bytes("/bin/crash", crash_elf_blob, crash_elf_blob_len);
        else
#endif
        {
            seed_file("/bin/crash", "MISSING_ELF\n");
        }
    }
    kprintf("vfs: initrd /bin /etc /tmp /proc /dev\n");
    return STATUS_SUCCESS;
}

status_t vfs_write_bytes(const char *path, const void *data, u64 n)
{
    file_object_t *f;
    status_t st = vfs_open(path, FILE_WRITE_DATA, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, &f);
    if (!NT_SUCCESS(st)) return st;
    u64 put = 0;
    st = vfs_write(f, data, n, &put);
    ob_dereference(&f->hdr);
    return st;
}

status_t vfs_read_all(const char *path, u8 **data, u64 *len)
{
    if (!path || !data || !len) return STATUS_INVALID_PARAMETER;
    file_object_t *f;
    status_t st = vfs_open(path, FILE_READ_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &f);
    if (!NT_SUCCESS(st)) return st;
    vnode_t *v = f->vnode;
    u8 *buf = kalloc(v->size + 1);
    if (!buf) {
        ob_dereference(&f->hdr);
        return STATUS_NO_MEMORY;
    }
    if (v->size && v->data) memcpy(buf, v->data, (size_t)v->size);
    buf[v->size] = 0;
    *data = buf;
    *len = v->size;
    ob_dereference(&f->hdr);
    return STATUS_SUCCESS;
}
