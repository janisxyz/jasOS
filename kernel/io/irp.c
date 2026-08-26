#include <jasos/io.h>
#include <jasos/mm.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>
#include <jasos/config.h>

static driver_object_t g_serial_drv;
static driver_object_t g_ramdisk_drv;
static device_object_t *g_ramdisk_dev;
static u64              g_ramdisk_size;

irp_t *io_alloc_irp(u32 major)
{
    irp_t *i = kalloc_zero(sizeof(*i));
    if (!i) return NULL;
    i->major = major;
    i->ref = 1;
    i->status = STATUS_PENDING;
    return i;
}

void io_free_irp(irp_t *irp)
{
    if (!irp) return;
    if (atomic_dec32(&irp->ref) == 1) kfree(irp);
}

void io_complete(irp_t *irp, status_t st, u64 info)
{
    irp->status = st;
    irp->information = info;
    if (irp->complete) irp->complete(irp);
}

status_t io_call_driver(device_object_t *dev, irp_t *irp)
{
    if (!dev || !irp || !dev->driver) return STATUS_INVALID_PARAMETER;
    if (irp->major >= IRP_MJ_MAX) return STATUS_INVALID_PARAMETER;
    drv_dispatch_fn fn = dev->driver->major[irp->major];
    if (!fn) return STATUS_NOT_IMPLEMENTED;
    irp->device = &dev->hdr;
    return fn(dev, irp);
}

status_t io_create_device(driver_object_t *drv, usize ext, const char *name, device_object_t **out)
{
    device_object_t *d = (device_object_t *)ob_create(ob_type_device(), name, NULL);
    if (!d) return STATUS_NO_MEMORY;
    d->driver = drv;
    d->ext_size = (u32)ext;
    if (ext) d->ext = kalloc_zero(ext);
    spin_init(&d->queue_lock, "devq", LOCK_RANK_VFS);
    list_init(&d->irp_queue);
    if (out) *out = d;
    return STATUS_SUCCESS;
}

static status_t serial_write_disp(device_object_t *dev, irp_t *irp)
{
    (void)dev;
    const char *s = irp->buffer;
    for (u64 i = 0; i < irp->length; i++) serial_putchar(s[i]);
    io_complete(irp, STATUS_SUCCESS, irp->length);
    return STATUS_SUCCESS;
}

void serial_device_register(void)
{
    memset(&g_serial_drv, 0, sizeof(g_serial_drv));
    g_serial_drv.name = "Serial";
    g_serial_drv.major[IRP_MJ_WRITE] = serial_write_disp;
    device_object_t *d;
    io_create_device(&g_serial_drv, 0, "Serial0", &d);
    (void)d;
}

typedef struct ramdisk_ext {
    u8        *base;
    u64        size;
    spinlock_t lock;
} ramdisk_ext_t;

static status_t ramdisk_create_disp(device_object_t *dev, irp_t *irp)
{
    (void)dev;
    io_complete(irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static status_t ramdisk_close_disp(device_object_t *dev, irp_t *irp)
{
    (void)dev;
    io_complete(irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static status_t ramdisk_rw(device_object_t *dev, irp_t *irp, int write)
{
    ramdisk_ext_t *x = dev ? (ramdisk_ext_t *)dev->ext : NULL;
    if (!x || !x->base) {
        io_complete(irp, STATUS_NO_SUCH_FILE, 0);
        return STATUS_NO_SUCH_FILE;
    }
    if (irp->offset >= x->size) {
        status_t st = write ? STATUS_INVALID_PARAMETER : STATUS_END_OF_FILE;
        io_complete(irp, st, 0);
        return st;
    }
    u64 n = irp->length;
    if (irp->offset + n > x->size) {
        if (write) {
            /* Fixed-size disk. Extending is a lie. */
            io_complete(irp, STATUS_DISK_FULL, 0);
            return STATUS_DISK_FULL;
        }
        n = x->size - irp->offset;
    }
    if (n && !irp->buffer) {
        io_complete(irp, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }
    spin_lock(&x->lock);
    if (write) memcpy(x->base + irp->offset, irp->buffer, (size_t)n);
    else       memcpy(irp->buffer, x->base + irp->offset, (size_t)n);
    spin_unlock(&x->lock);
    io_complete(irp, STATUS_SUCCESS, n);
    return STATUS_SUCCESS;
}

static status_t ramdisk_read_disp(device_object_t *dev, irp_t *irp)
{
    return ramdisk_rw(dev, irp, 0);
}

static status_t ramdisk_write_disp(device_object_t *dev, irp_t *irp)
{
    return ramdisk_rw(dev, irp, 1);
}

void ramdisk_init(void)
{
    memset(&g_ramdisk_drv, 0, sizeof(g_ramdisk_drv));
    g_ramdisk_drv.name = "Ramdisk";
    g_ramdisk_drv.major[IRP_MJ_CREATE] = ramdisk_create_disp;
    g_ramdisk_drv.major[IRP_MJ_CLOSE]  = ramdisk_close_disp;
    g_ramdisk_drv.major[IRP_MJ_READ]   = ramdisk_read_disp;
    g_ramdisk_drv.major[IRP_MJ_WRITE]  = ramdisk_write_disp;

    device_object_t *d = NULL;
    status_t st = io_create_device(&g_ramdisk_drv, sizeof(ramdisk_ext_t), "Ramdisk0", &d);
    if (!NT_SUCCESS(st) || !d) {
        kprintf("ramdisk: create failed %s\n", status_name(st));
        return;
    }
    ramdisk_ext_t *x = (ramdisk_ext_t *)d->ext;
    x->size = RAMDISK_SIZE;
    x->base = kalloc_zero((usize)x->size);
    spin_init(&x->lock, "ramdisk", LOCK_RANK_VFS);
    if (!x->base) {
        kprintf("ramdisk: backing store alloc failed\n");
        g_ramdisk_dev = d;
        g_ramdisk_size = 0;
        return;
    }
    g_ramdisk_dev = d;
    g_ramdisk_size = x->size;
    kprintf("ramdisk: Ramdisk0 %llu KiB backing at %p\n",
            (unsigned long long)(x->size / 1024), (void *)x->base);
}

device_object_t *ramdisk_device(void)
{
    return g_ramdisk_dev;
}

u64 ramdisk_size(void)
{
    return g_ramdisk_size;
}

void io_init(void)
{
    serial_device_register();
    ramdisk_init();
    kprintf("io: IRP model up, Serial0 + Ramdisk0 (%llu KiB)\n",
            (unsigned long long)(g_ramdisk_size / 1024));
}
