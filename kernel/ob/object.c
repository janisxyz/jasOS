#include <jasos/ke.h>
#include <jasos/fs.h>
#include <jasos/io.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>


static object_type_t g_types[OBJ_TYPE_MAX];
static directory_object_t *g_root;
static directory_object_t *g_bno;
static directory_object_t *g_devices;
static directory_object_t *g_types_dir;

static void nop_delete(object_t *o) { (void)o; }

static void type_init(object_kind_t k, const char *name, usize body, bool waitable,
                      access_t gr, access_t gw, access_t ge, access_t ga, obj_delete_fn del)
{
    g_types[k].name = name;
    g_types[k].kind = k;
    g_types[k].body_size = body;
    g_types[k].waitable = waitable;
    g_types[k].generic_read = gr;
    g_types[k].generic_write = gw;
    g_types[k].generic_execute = ge;
    g_types[k].generic_all = ga;
    g_types[k].delete_fn = del ? del : nop_delete;
}

object_type_t *ob_type_directory(void) { return &g_types[OBJ_DIRECTORY]; }
object_type_t *ob_type_process(void)   { return &g_types[OBJ_PROCESS]; }
object_type_t *ob_type_thread(void)    { return &g_types[OBJ_THREAD]; }
object_type_t *ob_type_section(void)   { return &g_types[OBJ_SECTION]; }
object_type_t *ob_type_file(void)      { return &g_types[OBJ_FILE]; }
object_type_t *ob_type_device(void)    { return &g_types[OBJ_DEVICE]; }
object_type_t *ob_type_event(void)     { return &g_types[OBJ_EVENT]; }
object_type_t *ob_type_mutex(void)     { return &g_types[OBJ_MUTEX]; }
object_type_t *ob_type_timer(void)     { return &g_types[OBJ_TIMER]; }

const char *ob_kind_name(object_kind_t k)
{
    if (k == 0 || k >= OBJ_TYPE_MAX || !g_types[k].name) return "?";
    return g_types[k].name;
}

static u32 hash_name(const char *s)
{
    u32 h = 2166136261u;
    while (*s) { h ^= (u8)*s++; h *= 16777619u; }
    return h % 37u;
}

static void dir_init_buckets(directory_object_t *d)
{
    spin_init(&d->lock, "dir", LOCK_RANK_OB);
    for (int i = 0; i < 37; i++) list_init(&d->buckets[i]);
    d->count = 0;
}

directory_object_t *ob_root(void) { return g_root; }
directory_object_t *ob_dir_bno(void) { return g_bno; }

access_t ob_map_generic(object_type_t *type, access_t access)
{
    access_t a = access;
    if (a & GENERIC_READ)    { a |= type->generic_read;    a &= ~GENERIC_READ; }
    if (a & GENERIC_WRITE)   { a |= type->generic_write;   a &= ~GENERIC_WRITE; }
    if (a & GENERIC_EXECUTE) { a |= type->generic_execute; a &= ~GENERIC_EXECUTE; }
    if (a & GENERIC_ALL)     { a |= type->generic_all;     a &= ~GENERIC_ALL; }
    return a;
}

object_t *ob_create(object_type_t *type, const char *name, directory_object_t *dir)
{
    if (!type) return NULL;
    object_t *o = kalloc_zero(sizeof(object_t) + type->body_size);
    if (!o) return NULL;
    o->type = type;
    o->pointer_count = 1;
    o->handle_count = 0;
    o->flags = type->waitable ? OBJ_WAITABLE : 0;
    o->owner_pid = ke_current_process() ? ke_current_process()->pid : 0;
    list_init(&o->dir_link);
    if (name) strlcpy(o->name, name, NAME_MAX);
    if (dir && name && name[0]) {
        status_t st = ob_insert_name(dir, o, name);
        if (!NT_SUCCESS(st)) {
            kfree(o);
            return NULL;
        }
    }
    return o;
}

void ob_reference(object_t *o)
{
    if (!o) return;
    atomic_inc64(&o->pointer_count);
}

void ob_dereference(object_t *o)
{
    if (!o) return;
    u64 old = __sync_fetch_and_sub(&o->pointer_count, 1);
    if (old == 0) panic("ob ref underflow %s", o->type ? o->type->name : "?");
    if (old == 1 && o->handle_count == 0 && !(o->flags & OBJ_PERMANENT)) {
        if (o->type && o->type->delete_fn) o->type->delete_fn(o);
        kfree(o);
    }
}

status_t ob_insert_name(directory_object_t *dir, object_t *o, const char *name)
{
    if (!dir || !o || !name || !name[0] || strlen(name) >= NAME_MAX)
        return STATUS_INVALID_PARAMETER;
    u32 h = hash_name(name);
    spin_lock(&dir->lock);
    list_t *b = &dir->buckets[h];
    for (list_t *e = b->next; e != b; e = e->next) {
        object_t *x = CONTAINER_OF(e, object_t, dir_link);
        if (strcmp(x->name, name) == 0) {
            spin_unlock(&dir->lock);
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    strlcpy(o->name, name, NAME_MAX);
    o->directory = &dir->hdr;
    o->dir_hash = h;
    list_insert_tail(b, &o->dir_link);
    dir->count++;
    spin_unlock(&dir->lock);
    return STATUS_SUCCESS;
}

static object_t *dir_lookup(directory_object_t *dir, const char *name)
{
    u32 h = hash_name(name);
    spin_lock(&dir->lock);
    list_t *b = &dir->buckets[h];
    for (list_t *e = b->next; e != b; e = e->next) {
        object_t *x = CONTAINER_OF(e, object_t, dir_link);
        if (strcmp(x->name, name) == 0) {
            ob_reference(x);
            spin_unlock(&dir->lock);
            return x;
        }
    }
    spin_unlock(&dir->lock);
    return NULL;
}

status_t ob_lookup(const char *path, object_kind_t expect, object_t **out)
{
    if (!path || !out || path[0] != '\\') return STATUS_OBJECT_NAME_INVALID;
    directory_object_t *d = g_root;
    const char *p = path + 1;
    if (*p == 0) {
        if (expect && expect != OBJ_DIRECTORY) return STATUS_OBJECT_TYPE_MISMATCH;
        ob_reference(&d->hdr);
        *out = &d->hdr;
        return STATUS_SUCCESS;
    }
    object_t *cur = &d->hdr;
    ob_reference(cur);
    char comp[NAME_MAX];
    while (*p) {
        u32 i = 0;
        while (*p && *p != '\\' && i + 1 < NAME_MAX) comp[i++] = *p++;
        comp[i] = 0;
        if (*p == '\\') p++;
        if (cur->type->kind != OBJ_DIRECTORY) {
            ob_dereference(cur);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        object_t *next = dir_lookup((directory_object_t *)cur, comp);
        ob_dereference(cur);
        if (!next) return STATUS_OBJECT_NAME_NOT_FOUND;
        cur = next;
    }
    if (expect && cur->type->kind != expect) {
        ob_dereference(cur);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *out = cur;
    return STATUS_SUCCESS;
}

static directory_object_t *make_dir(const char *name, directory_object_t *parent)
{
    object_t *o = ob_create(&g_types[OBJ_DIRECTORY], name, parent);
    if (!o) panic("ob: dir %s", name);
    directory_object_t *d = (directory_object_t *)o;
    /* body_size includes directory_object minus header... we allocated
       sizeof(object_t)+body. For Directory we pass full extra. */
    dir_init_buckets(d);
    o->flags |= OBJ_PERMANENT;
    return d;
}

void disp_init(dispatcher_t *d, disp_type_t type, i32 state)
{
    d->type = type;
    d->signal_state = state;
    list_init(&d->wait_list);
    spin_init(&d->lock, "disp", LOCK_RANK_DISP);
}

void disp_wake_one(dispatcher_t *d, status_t st)
{
    if (list_empty(&d->wait_list)) return;
    list_t *e = d->wait_list.next;
    wait_block_t *wb = CONTAINER_OF(e, wait_block_t, obj_link);
    list_remove(&wb->obj_link);
    wb->wake_status = st;
    if (wb->thread)
        wb->thread->wait.wake_status = st;
    if (d->type == DISP_MUTANT) {
        mutex_object_t *m = CONTAINER_OF(d, mutex_object_t, disp);
        m->owner = wb->thread;
        m->recursion = 1;
        m->abandoned = (st == STATUS_ABANDONED);
        d->signal_state = 0;
    }
    if (wb->thread && wb->thread->state == THR_WAITING) {
        /* sched_ready takes SCHED. Caller may hold DISP (rank 9).
           DISP < SCHED is the T3 contract. */
        sched_ready(wb->thread);
    }
}

void disp_wake_all(dispatcher_t *d, status_t st)
{
    while (!list_empty(&d->wait_list)) disp_wake_one(d, st);
}

void disp_signal(dispatcher_t *d, i32 increment)
{
    spin_lock(&d->lock);
    d->signal_state += increment;
    if (d->type == DISP_NOTIFICATION_EVENT) {
        disp_wake_all(d, STATUS_SUCCESS);
    } else {
        while (d->signal_state > 0 && !list_empty(&d->wait_list)) {
            disp_wake_one(d, STATUS_SUCCESS);
            if (d->type == DISP_SYNCHRONIZATION_EVENT || d->type == DISP_MUTANT)
                d->signal_state--;
        }
    }
    spin_unlock(&d->lock);
}

status_t ob_create_event(const char *name, bool auto_reset, bool initial, event_object_t **out)
{
    object_t *o = ob_create(&g_types[OBJ_EVENT], name, name ? g_bno : NULL);
    if (!o) return STATUS_NO_MEMORY;
    event_object_t *e = (event_object_t *)o;
    e->auto_reset = auto_reset;
    disp_init(&e->disp, auto_reset ? DISP_SYNCHRONIZATION_EVENT : DISP_NOTIFICATION_EVENT,
              initial ? 1 : 0);
    o->wait = &e->disp;
    *out = e;
    return STATUS_SUCCESS;
}

status_t ob_create_mutex(const char *name, bool initial_owner, mutex_object_t **out)
{
    object_t *o = ob_create(&g_types[OBJ_MUTEX], name, name ? g_bno : NULL);
    if (!o) return STATUS_NO_MEMORY;
    mutex_object_t *m = (mutex_object_t *)o;
    disp_init(&m->disp, DISP_MUTANT, initial_owner ? 0 : 1);
    m->owner = initial_owner ? ke_current() : NULL;
    m->recursion = initial_owner ? 1 : 0;
    o->wait = &m->disp;
    *out = m;
    return STATUS_SUCCESS;
}

void ob_init(void)
{
    memset(g_types, 0, sizeof(g_types));
    type_init(OBJ_DIRECTORY, "Directory", sizeof(directory_object_t) - sizeof(object_t), false,
              DIRECTORY_QUERY, DIRECTORY_CREATE_OBJECT, DIRECTORY_TRAVERSE, 0x1F, NULL);
    type_init(OBJ_PROCESS, "Process", sizeof(process_t) - sizeof(object_t), true,
              PROCESS_QUERY_INFORMATION, PROCESS_VM_WRITE, PROCESS_CREATE_THREAD, PROCESS_ALL_ACCESS, NULL);
    type_init(OBJ_THREAD, "Thread", sizeof(thread_t) - sizeof(object_t), true,
              THREAD_QUERY_INFORMATION, THREAD_SUSPEND_RESUME, 0, THREAD_ALL_ACCESS, NULL);
    type_init(OBJ_SECTION, "Section", sizeof(section_object_t) - sizeof(object_t), false,
              SECTION_MAP_READ, SECTION_MAP_WRITE, SECTION_MAP_EXECUTE, SECTION_ALL_ACCESS, NULL);
    type_init(OBJ_FILE, "File", sizeof(file_object_t) - sizeof(object_t), false,
              FILE_READ_DATA, FILE_WRITE_DATA, FILE_EXECUTE, FILE_ALL_ACCESS, NULL);
    type_init(OBJ_DEVICE, "Device", sizeof(device_object_t) - sizeof(object_t), false,
              FILE_READ_DATA, FILE_WRITE_DATA, 0, FILE_ALL_ACCESS, NULL);
    type_init(OBJ_EVENT, "Event", sizeof(event_object_t) - sizeof(object_t), true,
              SYNCHRONIZE, EVENT_MODIFY_STATE, 0, SYNCHRONIZE | EVENT_MODIFY_STATE, NULL);
    type_init(OBJ_MUTEX, "Mutant", sizeof(mutex_object_t) - sizeof(object_t), true,
              SYNCHRONIZE, MUTEX_MODIFY_STATE, 0, SYNCHRONIZE | MUTEX_MODIFY_STATE, NULL);
    type_init(OBJ_TIMER, "Timer", sizeof(timer_object_t) - sizeof(object_t), true,
              SYNCHRONIZE, TIMER_MODIFY_STATE, 0, SYNCHRONIZE | TIMER_MODIFY_STATE, NULL);

    /* Root is not allocated through ob_create (chicken/egg). */
    g_root = kalloc_zero(sizeof(*g_root));
    if (!g_root) panic("ob: root");
    g_root->hdr.type = &g_types[OBJ_DIRECTORY];
    g_root->hdr.pointer_count = 1;
    g_root->hdr.flags = OBJ_PERMANENT;
    strlcpy(g_root->hdr.name, "\\", NAME_MAX);
    dir_init_buckets(g_root);

    g_types_dir = make_dir("ObjectTypes", g_root);
    g_bno       = make_dir("BaseNamedObjects", g_root);
    g_devices   = make_dir("Devices", g_root);
    make_dir("??", g_root);
    kprintf("ob: namespace \\ ObjectTypes BaseNamedObjects Devices ??\n");
}
