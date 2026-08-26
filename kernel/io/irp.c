#include <jasos/io.h>
#include <jasos/mm.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>

static driver_object_t g_serial_drv;
static driver_object_t g_ramdisk_drv;

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

void io_init(void)
{
    memset(&g_ramdisk_drv, 0, sizeof(g_ramdisk_drv));
    g_ramdisk_drv.name = "Ramdisk";
    serial_device_register();
    kprintf("io: IRP model up, Serial0 + Ramdisk\n");
}
