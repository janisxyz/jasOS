#pragma once

#include <jasos/types.h>
#include <jasos/status.h>
#include <jasos/ob.h>

#define IRP_MJ_CREATE   0
#define IRP_MJ_CLOSE    1
#define IRP_MJ_READ     2
#define IRP_MJ_WRITE    3
#define IRP_MJ_DEVICE   4
#define IRP_MJ_CLEANUP  5
#define IRP_MJ_MAX      6

typedef struct irp {
    u32              major;
    u32              flags;
    status_t         status;
    u64              information;
    void            *buffer;
    u64              length;
    u64              offset;
    object_t        *device;
    object_t        *file;
    void           (*complete)(struct irp *);
    bool             cancelled;
    volatile u32     ref;
} irp_t;

typedef struct device_object device_object_t;

typedef status_t (*drv_dispatch_fn)(device_object_t *dev, irp_t *irp);

typedef struct driver_object {
    const char      *name;
    drv_dispatch_fn  major[IRP_MJ_MAX];
    void           (*unload)(struct driver_object *);
} driver_object_t;

struct device_object {
    object_t         hdr;
    driver_object_t *driver;
    void            *ext;
    spinlock_t       queue_lock;
    list_t           irp_queue;
    u32              ext_size;
};

irp_t  *io_alloc_irp(u32 major);
void    io_free_irp(irp_t *irp);
void    io_complete(irp_t *irp, status_t st, u64 info);
status_t io_call_driver(device_object_t *dev, irp_t *irp);
status_t io_create_device(driver_object_t *drv, usize ext, const char *name, device_object_t **out);
void    io_init(void);
void    serial_device_register(void);
void    ramdisk_init(void);
struct device_object *ramdisk_device(void);
u64     ramdisk_size(void);
