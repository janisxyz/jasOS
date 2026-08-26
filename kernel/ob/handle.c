#include <jasos/ob.h>
#include <jasos/string.h>
#include <jasos/kprintf.h>

void ht_init(handle_table_t *t)
{
    memset(t, 0, sizeof(*t));
    spin_init(&t->lock, "handle", LOCK_RANK_HANDLE);
}

void ht_destroy(handle_table_t *t)
{
    spin_lock(&t->lock);
    for (u32 i = 1; i < HANDLE_TABLE_SLOTS; i++) {
        if (t->slots[i].object) {
            object_t *o = t->slots[i].object;
            t->slots[i].object = NULL;
            atomic_dec64(&o->handle_count);
            ob_dereference(o);
        }
    }
    t->used = 0;
    spin_unlock(&t->lock);
}

status_t ht_insert(handle_table_t *t, object_t *o, access_t access, handle_t *out)
{
    if (!t || !o || !out) return STATUS_INVALID_PARAMETER;
    access = ob_map_generic(o->type, access);
    spin_lock(&t->lock);
    for (u32 i = 1; i < HANDLE_TABLE_SLOTS; i++) {
        if (!t->slots[i].object) {
            ob_reference(o);
            atomic_inc64(&o->handle_count);
            t->slots[i].object = o;
            t->slots[i].access = access;
            t->slots[i].inherit = 0;
            t->slots[i].protect_close = 0;
            t->slots[i].generation++;
            t->used++;
            *out = HANDLE_VALUE(i);
            spin_unlock(&t->lock);
            return STATUS_SUCCESS;
        }
    }
    spin_unlock(&t->lock);
    return STATUS_INSUFFICIENT_RESOURCES;
}

status_t ht_lookup(handle_table_t *t, handle_t h, access_t required, object_kind_t expect, object_t **out)
{
    if (!t || !out) return STATUS_INVALID_PARAMETER;
    if (h == 0 || (h & 3u) != 0) return STATUS_INVALID_HANDLE;
    u32 i = HANDLE_INDEX(h);
    if (i == 0 || i >= HANDLE_TABLE_SLOTS) return STATUS_INVALID_HANDLE;
    spin_lock(&t->lock);
    handle_entry_t *e = &t->slots[i];
    if (!e->object) {
        spin_unlock(&t->lock);
        return STATUS_INVALID_HANDLE;
    }
    if (required && (e->access & required) != required) {
        spin_unlock(&t->lock);
        return STATUS_ACCESS_DENIED;
    }
    if (expect && e->object->type->kind != expect) {
        spin_unlock(&t->lock);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    ob_reference(e->object);
    *out = e->object;
    spin_unlock(&t->lock);
    return STATUS_SUCCESS;
}

status_t ht_close(handle_table_t *t, handle_t h)
{
    if (!t) return STATUS_INVALID_PARAMETER;
    u32 i = HANDLE_INDEX(h);
    if (i == 0 || i >= HANDLE_TABLE_SLOTS || (h & 3u) != 0) return STATUS_INVALID_HANDLE;
    spin_lock(&t->lock);
    handle_entry_t *e = &t->slots[i];
    if (!e->object) {
        spin_unlock(&t->lock);
        return STATUS_INVALID_HANDLE;
    }
    if (e->protect_close) {
        spin_unlock(&t->lock);
        return STATUS_ACCESS_DENIED;
    }
    object_t *o = e->object;
    e->object = NULL;
    e->access = 0;
    e->generation++;
    t->used--;
    spin_unlock(&t->lock);
    atomic_dec64(&o->handle_count);
    ob_dereference(o);
    return STATUS_SUCCESS;
}

status_t ht_duplicate(handle_table_t *src, handle_t h, handle_table_t *dst, access_t access, handle_t *out)
{
    if (!src || !dst || !out) return STATUS_INVALID_PARAMETER;
    object_t *o;
    status_t st = ht_lookup(src, h, 0, 0, &o);
    if (!NT_SUCCESS(st)) return st;
    if (access == 0) {
        u32 i = HANDLE_INDEX(h);
        access = src->slots[i].access;
    }
    st = ht_insert(dst, o, access, out);
    ob_dereference(o);
    return st;
}
