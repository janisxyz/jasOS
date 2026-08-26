#pragma once

#include <jasos/types.h>
#include <jasos/status.h>
#include <jasos/ob.h>
#include <jasos/config.h>

#define VNODE_FILE 1
#define VNODE_DIR  2
#define VNODE_CHAR 3
#define VNODE_BLOCK 4

struct device_object;

typedef struct vnode {
    u32            kind;
    u32            mode;
    u64            size;
    u64            mtime;
    char           name[NAME_MAX];
    struct vnode  *parent;
    list_t         children;
    list_t         sibling;
    u8            *data;
    u64            cap;
    spinlock_t     lock;
    volatile u64   ref;
    struct device_object *device;
} vnode_t;

typedef struct file_object {
    object_t  hdr;
    vnode_t  *vnode;
    u64       offset;
    access_t  access;
    u32       flags;
} file_object_t;

typedef struct vfs_mount {
    vnode_t *root;
    char     path[PATH_MAX];
} vfs_mount_t;

void     vfs_init(void);
status_t vfs_mkdir(const char *path);
status_t vfs_create(const char *path, file_object_t **out);
status_t vfs_open(const char *path, access_t access, u32 disp, u32 opts, file_object_t **out);
status_t vfs_read(file_object_t *f, void *buf, u64 n, u64 *got);
status_t vfs_write(file_object_t *f, const void *buf, u64 n, u64 *put);
status_t vfs_readdir(file_object_t *f, char *buf, u64 cap, u64 *put, bool restart);
status_t vfs_stat_path(const char *path, vnode_t **out);
status_t vfs_unlink(const char *path);
void     vfs_file_delete(object_t *o);
status_t vfs_read_all(const char *path, u8 **data, u64 *len);
status_t vfs_write_bytes(const char *path, const void *data, u64 n);
vnode_t *vfs_root(void);
status_t vfs_seed_initrd(void);
void     ramfs_init(vnode_t **root_out);

status_t path_norm(const char *cwd, const char *in, char *out, usize cap);
