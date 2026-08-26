#include <jasos/ke.h>
#include <jasos/kprintf.h>
#include <jasos/string.h>

typedef struct PACKED {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  gran;
    u8  base_high;
} gdt_entry_t;

typedef struct PACKED {
    u16 len;
    u64 ptr;
} gdtr_t;

typedef struct PACKED {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap;
} tss_t;

static gdt_entry_t gdt[8];
static gdtr_t      gdtr;
static tss_t       tss;
static u8          ist1[4096] ALIGNED(16);
static u8          ist2[4096] ALIGNED(16);
static u8          ist3[4096] ALIGNED(16);

static void gdt_set(int i, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[i].limit_low = (u16)(limit & 0xFFFF);
    gdt[i].base_low  = (u16)(base & 0xFFFF);
    gdt[i].base_mid  = (u8)((base >> 16) & 0xFF);
    gdt[i].access    = access;
    gdt[i].gran      = (u8)((limit >> 16) & 0x0F) | gran;
    gdt[i].base_high = (u8)((base >> 24) & 0xFF);
}

void gdt_init(void)
{
    memset(gdt, 0, sizeof(gdt));
    gdt_set(1, 0, 0, 0x9A, 0x20); /* kernel code 64 L=1 */
    gdt_set(2, 0, 0, 0x92, 0x00); /* kernel data */
    gdt_set(3, 0, 0, 0xF2, 0x00); /* user data */
    gdt_set(4, 0, 0, 0xFA, 0x20); /* user code 64 */
    gdtr.len = sizeof(gdt) - 1;
    gdtr.ptr = (u64)&gdt;
    __asm__ volatile("lgdt %0" :: "m"(gdtr));
    kprintf("gdt: kcode=08 kdata=10 ucode=20 udata=18\n");
}

void tss_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.ist[0] = (u64)(ist1 + sizeof(ist1));
    tss.ist[1] = (u64)(ist2 + sizeof(ist2));
    tss.ist[2] = (u64)(ist3 + sizeof(ist3));
    tss.iomap = sizeof(tss);
    u64 b = (u64)&tss;
    gdt_set(5, (u32)b, sizeof(tss) - 1, 0x89, 0);
    /* 64-bit TSS is 16 bytes: extra high base in entry 6 */
    gdt[6].limit_low = (u16)(b >> 32);
    gdt[6].base_low  = (u16)(b >> 48);
    __asm__ volatile("ltr %0" :: "r"((u16)0x28));
    kprintf("tss: IST1-3 armed for DF/NMI/MC\n");
}
