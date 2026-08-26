#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/types.h>

/*
 * PCI config probe. Type-1 (0xCF8/0xCFC). Bus 0 only in v0.2.
 *
 * Why this will fail in production:
 *  - No BAR decode, no MSI, no bus walk past 0. A device behind a
 *    bridge is invisible. That is the next HAL commit, not this one.
 */

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

void pci_init(void)
{
    u32 found = 0;
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
    }
    kprintf("pci: %u device(s) on bus 0\n", found);
}
