#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/types.h>

/*
 * PCI config probe. Type-1 (0xCF8/0xCFC). Bus 0 only in v0.2.
 *
 * Why this will fail in production:
 *  - No BAR decode beyond virtio-blk legacy capacity, no MSI, no bus
 *    walk past 0. A device behind a bridge is invisible.
 *  - Virtio 1.0 (modern, device 0x1042) needs the PCI cap list and
 *    virtqueues. This pass identifies the device and, for legacy
 *    0x1001, reads capacity. It does not claim I/O. Ramdisk0 is the
 *    block device that actually stores bytes.
 */

#define VIRTIO_VENDOR           0x1AF4u
#define VIRTIO_DEV_BLK_LEGACY   0x1001u
#define VIRTIO_DEV_BLK_MODERN   0x1042u
#define PCI_BAR0                0x10u
#define PCI_SUBSYSTEM           0x2Cu
#define VIRTIO_LEGACY_CFG       20u /* device-specific config after 20-byte header */

static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline u32  inl(u16 p) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

u32 pci_read(u8 bus, u8 slot, u8 func, u8 offset)
{
    u32 addr = (u32)(1u << 31)
             | ((u32)bus << 16)
             | ((u32)slot << 11)
             | ((u32)func << 8)
             | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void virtio_blk_probe(u8 slot, u16 device)
{
    u32 bar0 = pci_read(0, slot, 0, PCI_BAR0);
    u32 sub  = pci_read(0, slot, 0, PCI_SUBSYSTEM);
    kprintf("virtio-blk: 00:%02x.0 %s bar0=%08x subsys=%08x\n",
            slot, device == VIRTIO_DEV_BLK_LEGACY ? "legacy" : "modern",
            bar0, sub);
    if (device == VIRTIO_DEV_BLK_LEGACY && (bar0 & 1u)) {
        u16 iobase = (u16)(bar0 & ~3u);
        u32 lo = inl((u16)(iobase + VIRTIO_LEGACY_CFG));
        u32 hi = inl((u16)(iobase + VIRTIO_LEGACY_CFG + 4));
        u64 cap = ((u64)hi << 32) | lo;
        kprintf("virtio-blk: legacy capacity %llu sectors (%llu KiB)\n",
                (unsigned long long)cap,
                (unsigned long long)(cap / 2));
    } else {
        kprintf("virtio-blk: modern/mmio — virtqueues not claimed\n");
    }
    kprintf("virtio-blk: identified; I/O stays on Ramdisk0 this pass\n");
}

void pci_init(void)
{
    u32 found = 0;
    u32 virtio_blk = 0;
    for (u8 slot = 0; slot < 32; slot++) {
        u32 id = pci_read(0, slot, 0, 0);
        u16 vendor = (u16)id;
        u16 device = (u16)(id >> 16);
        if (vendor == 0xFFFF) continue;
        u32 cls = pci_read(0, slot, 0, 0x08);
        u8 class = (u8)(cls >> 24);
        u8 sub   = (u8)(cls >> 16);
        kprintf("pci: 00:%02x.0 vendor %04x device %04x class %02x%02x\n",
                slot, vendor, device, class, sub);
        found++;
        if (vendor == VIRTIO_VENDOR &&
            (device == VIRTIO_DEV_BLK_LEGACY || device == VIRTIO_DEV_BLK_MODERN)) {
            virtio_blk_probe(slot, device);
            virtio_blk++;
        }
    }
    kprintf("pci: %u device(s) on bus 0, virtio-blk %u\n", found, virtio_blk);
}
