#include <jasos/ob.h>
#include <jasos/mm.h>
#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>

/*
 * Byte pipe. Waitable for readers when data is present, for writers when
 * space is present. One 4 KiB ring.
 *
 * Why this will fail in production:
 *  - No O_NONBLOCK. A full pipe sleeps the writer.
 *  - No writer-count; read after last close does not yet yield EOF
 *    unless both ends called NtClose on a pipe with a flag we do not
 *    store. Residual: close-from-writer EOF is next.
 */

#define PIPE_CAP 4096

typedef struct pipe_object {
    object_t     hdr;
    dispatcher_t can_read;
    dispatcher_t can_write;
    u8           buf[PIPE_CAP];
    u32          r, w, used;
    spinlock_t   lock;
    u32          readers;
    u32          writers;
    bool         writer_closed;
} pipe_object_t;

static object_type_t g_pipe_type;

static void pipe_delete(object_t *o)
{
    (void)o;
}

static void pipe_close(object_t *o, access_t acc)
{
    pipe_object_t *p = (pipe_object_t *)o;
    spin_lock(&p->lock);
    if (acc & FILE_WRITE_DATA) {
        if (p->writers) p->writers--;
        if (p->writers == 0) p->writer_closed = true;
    }
    if (acc & FILE_READ_DATA) {
        if (p->readers) p->readers--;
    }
    int eof = p->writer_closed;
    spin_unlock(&p->lock);
    if (eof) disp_signal(&p->can_read, 1);
}

void pipe_init_type(void)
{
    g_pipe_type.name = "Pipe";
    g_pipe_type.kind = OBJ_PIPE;
    g_pipe_type.body_size = sizeof(pipe_object_t) - sizeof(object_t);
    g_pipe_type.generic_read = FILE_READ_DATA;
    g_pipe_type.generic_write = FILE_WRITE_DATA;
    g_pipe_type.generic_all = FILE_ALL_ACCESS;
    g_pipe_type.delete_fn = pipe_delete;
    g_pipe_type.close_fn = pipe_close;
    g_pipe_type.waitable = true;
}

status_t NtCreatePipe(handle_t *read_out, handle_t *write_out)
{
    if (!read_out || !write_out) return STATUS_INVALID_PARAMETER;
    if (!g_pipe_type.name) pipe_init_type();
    pipe_object_t *p = (pipe_object_t *)ob_create(&g_pipe_type, NULL, NULL);
    if (!p) return STATUS_NO_MEMORY;
    disp_init(&p->can_read, DISP_NOTIFICATION_EVENT, 0);
    disp_init(&p->can_write, DISP_NOTIFICATION_EVENT, 1);
    p->hdr.wait = &p->can_read;
    spin_init(&p->lock, "pipe", LOCK_RANK_OB);
    p->r = p->w = p->used = 0;
    p->readers = 1;
    p->writers = 1;
    p->writer_closed = false;
    handle_table_t *t = ke_current_process() ? &ke_current_process()->handles : NULL;
    if (!t) { ob_dereference(&p->hdr); return STATUS_INVALID_HANDLE; }
    status_t st = ht_insert(t, &p->hdr, FILE_READ_DATA, read_out);
    if (!NT_SUCCESS(st)) { ob_dereference(&p->hdr); return st; }
    st = ht_insert(t, &p->hdr, FILE_WRITE_DATA, write_out);
    ob_dereference(&p->hdr);
    return st;
}

status_t pipe_read(object_t *o, void *buf, u64 n, u64 *got)
{
    pipe_object_t *p = (pipe_object_t *)o;
    u8 *d = buf;
    u64 done = 0;
    while (done < n) {
        spin_lock(&p->lock);
        if (p->used == 0) {
            int eof = p->writer_closed;
            spin_unlock(&p->lock);
            if (done) break;
            if (eof) break;
            ke_wait_object(&p->can_read, (u64)-1);
            continue;
        }
        u32 take = (u32)MIN(n - done, (u64)p->used);
        for (u32 i = 0; i < take; i++) {
            d[done++] = p->buf[p->r];
            p->r = (p->r + 1) % PIPE_CAP;
            p->used--;
        }
        if (p->used == 0) p->can_read.signal_state = 0;
        p->can_write.signal_state = 1;
        disp_wake_one(&p->can_write, STATUS_SUCCESS);
        spin_unlock(&p->lock);
    }
    if (got) *got = done;
    return done ? STATUS_SUCCESS : STATUS_END_OF_FILE;
}

status_t pipe_write(object_t *o, const void *buf, u64 n, u64 *put)
{
    pipe_object_t *p = (pipe_object_t *)o;
    const u8 *s = buf;
    u64 done = 0;
    while (done < n) {
        spin_lock(&p->lock);
        if (p->used == PIPE_CAP) {
            spin_unlock(&p->lock);
            ke_wait_object(&p->can_write, (u64)-1);
            continue;
        }
        u32 room = PIPE_CAP - p->used;
        u32 take = (u32)MIN(n - done, (u64)room);
        for (u32 i = 0; i < take; i++) {
            p->buf[p->w] = s[done++];
            p->w = (p->w + 1) % PIPE_CAP;
            p->used++;
        }
        p->can_read.signal_state = 1;
        disp_wake_one(&p->can_read, STATUS_SUCCESS);
        if (p->used < PIPE_CAP) p->can_write.signal_state = 1;
        else p->can_write.signal_state = 0;
        spin_unlock(&p->lock);
    }
    if (put) *put = done;
    return STATUS_SUCCESS;
}
