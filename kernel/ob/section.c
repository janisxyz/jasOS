#include <jasos/ob.h>
#include <jasos/ke.h>
#include <jasos/mm.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/status.h>

/*
 * Section objects. v1 is pagefile-backed (kernel heap), not file-backed.
 * NtMapViewOfSection copies into the target aspace. MEMORY.md: this is
 * a performance crime and a security simplification until the pager.
 */

static void section_delete(object_t *o)
{
    section_object_t *s = (section_object_t *)o;
    if (s->kdata) {
        kfree(s->kdata);
        s->kdata = NULL;
    }
}

static int g_section_delete_wired;

static void wire_section_delete(void)
{
    if (g_section_delete_wired) return;
    object_type_t *t = ob_type_section();
    t->delete_fn = section_delete;
    g_section_delete_wired = 1;
}

status_t NtCreateSection(handle_t *out, access_t access, u64 size, u32 prot)
{
    if (!out || size == 0) return STATUS_INVALID_PARAMETER;
    if (size > 16ull * 1024ull * 1024ull) return STATUS_INVALID_PARAMETER;
    wire_section_delete();
    object_t *o = ob_create(ob_type_section(), NULL, NULL);
    if (!o) return STATUS_NO_MEMORY;
    section_object_t *s = (section_object_t *)o;
    s->size = PAGE_ALIGN_UP(size);
    s->prot = prot ? prot : PAGE_READWRITE;
    s->kdata = kalloc_zero(s->size);
    if (!s->kdata) {
        ob_dereference(o);
        return STATUS_NO_MEMORY;
    }
    process_t *p = ke_current_process();
    if (!p) {
        ob_dereference(o);
        return STATUS_INVALID_HANDLE;
    }
    access_t acc = access ? access : SECTION_ALL_ACCESS;
    status_t st = ht_insert(&p->handles, o, acc, out);
    ob_dereference(o);
    return st;
}

status_t NtMapViewOfSection(handle_t section, handle_t proc, virt_t *base, u64 size, u32 prot)
{
    process_t *cur = ke_current_process();
    if (!cur || !base) return STATUS_INVALID_PARAMETER;
    object_t *so;
    status_t st = ht_lookup(&cur->handles, section, SECTION_MAP_READ, OBJ_SECTION, &so);
    if (!NT_SUCCESS(st)) {
        st = ht_lookup(&cur->handles, section, 0, OBJ_SECTION, &so);
        if (!NT_SUCCESS(st)) return st;
    }
    section_object_t *s = (section_object_t *)so;
    process_t *p;
    if (proc == HANDLE_CURRENT) p = cur;
    else {
        object_t *po;
        st = ht_lookup(&cur->handles, proc, PROCESS_VM_OPERATION, OBJ_PROCESS, &po);
        if (!NT_SUCCESS(st)) {
            ob_dereference(so);
            return st;
        }
        p = (process_t *)po;
        ob_dereference(po);
    }
    u64 map_size = size ? PAGE_ALIGN_UP(size) : s->size;
    if (map_size > s->size) map_size = s->size;
    u32 pr = prot ? prot : s->prot;
    virt_t va = *base;
    st = vmm_alloc_user(p, &va, map_size, pr, MEM_COMMIT);
    if (!NT_SUCCESS(st)) {
        ob_dereference(so);
        return st;
    }
    st = vmm_write_aspace(&p->aspace, va, s->kdata, map_size);
    *base = va;
    ob_dereference(so);
    return st;
}

status_t NtUnmapViewOfSection(handle_t proc, virt_t base)
{
    process_t *cur = ke_current_process();
    if (!cur) return STATUS_INVALID_HANDLE;
    process_t *p;
    if (proc == HANDLE_CURRENT) p = cur;
    else {
        object_t *po;
        status_t st = ht_lookup(&cur->handles, proc, PROCESS_VM_OPERATION, OBJ_PROCESS, &po);
        if (!NT_SUCCESS(st)) return st;
        p = (process_t *)po;
        ob_dereference(po);
    }
    return vmm_free_user(p, base, PAGE_SIZE);
}
