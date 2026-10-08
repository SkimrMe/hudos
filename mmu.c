/* hudos Phase A: EL1 exception handling / MMU foundation.
 *
 * NOTE (2026-09-13): Phase A is currently a BOOT-SAFE NO-OP. The original plan
 * was to install a custom EL1 exception vector table, but overriding the
 * firmware's VBAR_EL1 while UEFI boot services are still alive made the system
 * hang silently right after the banner (the CPU spun in our IRQ stub, which
 * `eret`s without acknowledging the interrupt). See hudos_mmu_init() for the
 * full explanation. The page-table builder + vector-table code below is retained
 * as the foundation for Phase B/C (EL0 drop via a TTBR0 switch), where custom
 * vectors can be installed safely AFTER ExitBootServices.
 *
 * Userland still calls the kernel via the existing `bl __syscall` function-pointer
 * convention. We stay in EL1 and keep the firmware's 1:1 MMU. */
#include "efi.h"

typedef unsigned long long u64;

/* PL011 (QEMU virt) for guaranteed debug output.
 * IMPORTANT: the busy-wait on TXFF must be BOUNDED. If the UART stops draining
 * (e.g. QEMU serial back-pressure), an unbounded spin would silently hang the
 * whole boot after the last successful print. Bound it so a stuck FIFO drops the
 * char instead of deadlocking the system. */
#define PL011_BASE 0x9000000ULL
static void uart_putc(char c) {
    volatile unsigned int *fr = (volatile unsigned int *)(PL011_BASE + 0x18);
    volatile unsigned int *dr = (volatile unsigned int *)(PL011_BASE + 0x00);
    for (int spin = 0; spin < 1000000; spin++) {
        if (!(*fr & (1u << 5))) break;   /* TXFF clear -> ready */
    }
    *dr = (unsigned int)(unsigned char)c;
}
static void uart_print(const char *s) {
    if (!s) return;
    while (*s) uart_putc(*s++);
}
static void uart_print_hex(const char *label, u64 v) {
    char buf[40]; int i = 0;
    while (label[i]) buf[i] = label[i], i++;
    buf[i++] = '0'; buf[i++] = 'x';
    for (int s = 60; s >= 0; s -= 4) {
        int d = (int)((v >> s) & 0xF);
        buf[i++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
    }
    buf[i++] = '\r'; buf[i++] = '\n'; buf[i] = 0;
    uart_print(buf);
}

/* EFI_MEMORY_DESCRIPTOR is not in efi.h; define the standard layout. */
typedef struct {
    UINT32 Type;
    UINT32 Pad;
    UINTN  PhysicalStart;
    UINTN  VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);

/* ===========================================================================
 * Phase B/C foundation (currently INACTIVE — retained for later use).
 * These build an identity-map EL1 page table and a fault handler. They are NOT
 * invoked by hudos_mmu_init() today (see the no-op below). Kept so Phase B/C can
 * switch TTBR0 to an EL0 table without re-deriving the descriptor math. */
/* ========================================================================= */

/* Page table: 3-level identity map (L0 -> L1 -> L2), 4KB granule.
 *   L0 (g_l0) : 512 entries; entry 0 -> g_l1a  (covers 0..512GB)
 *   L1 (g_l1a): 512 entries; entry i -> g_l2[i] (each covers 1GB)
 *   L2 (g_l2[i]): 512 entries of 2MB blocks (bits[29:21])
 * T0SZ=16 (48-bit VA) => the walk starts at L0; an L2 descriptor is a 2MB block. */
#define L2_SLOTS 64                 /* L1 slots we populate = up to 64GB */
#define L2_BLOCK 0x200000ULL        /* 2MB */
/* Page tables live INSIDE the loaded EFI image (which UEFI maps 1:1, VA==PA),
 * as a statically-allocated, 4KB-aligned array. This avoids AllocatePages, whose
 * returned address is a *virtual* address that the UEFI MMU may map to a
 * DIFFERENT physical page. */
#define PT_NPAGES (L2_SLOTS + 2)    /* L0(1) + L1(1) + L2(L2_SLOTS) */
static u64 g_pt_mem[PT_NPAGES * 512] __attribute__((aligned(0x1000)));
static u64 *g_l0 = 0;
static u64 *g_l1a = 0;
static u64 *g_l2[L2_SLOTS];

static int is_device(int type) {
    return type == EfiMemoryMappedIO || type == EfiMemoryMappedIOPortSpace ||
           type == EfiUnusableMemory;
}

static void map_block(u64 pa, int device) {
    int l1 = (int)((pa >> 30) & 0x1FF);   /* L1 index (1GB) */
    if (l1 >= L2_SLOTS) return;            /* beyond supported range */
    int l2 = (int)((pa >> 21) & 0x1FF);   /* L2 index (2MB) */
    /* L2 block descriptor: type=block(0b01), AF=1, SH=IS(0b11), AP=EL1 RW(0b01) */
    u64 e = pa | 0x1 | (1ULL << 10) | (3ULL << 8) | (1ULL << 6);
    if (device) {
        /* Device-nGnRE (AttrIndex=1), plus XN so no code executes from device mem */
        e = pa | 0x1 | (1ULL << 10) | (1ULL << 6) | (1ULL << 2) | (1ULL << 54);
    }
    g_l2[l1][l2] = e;
}

/* Under the UEFI 1:1 map the C pointer value (VA) IS already the physical
 * address, so identity translation is correct. */
static u64 va_to_pa(EFI_MEMORY_DESCRIPTOR *mm, int n, u64 va) {
    (void)mm; (void)n;
    return va;
}

/* ---- C handler called from the vector table (Phase B/C) ----
 * Reads the fault registers for whichever EL we are at and prints ESR/FAR/ELR to
 * the PL011 UART so MMU problems are debuggable. */
void hudos_mmu_fault_entry(void) {
    u64 esr, far, elr, spsr, el;
    __asm__ volatile ("mrs %0, currentel" : "=r"(el));
    if ((el & 0xC) == 0x8) {           /* EL2 */
        __asm__ volatile ("mrs %0, esr_el2; mrs %1, far_el2; mrs %2, elr_el2; mrs %3, spsr_el2"
                          : "=r"(esr), "=r"(far), "=r"(elr), "=r"(spsr));
    } else {                           /* EL1 */
        __asm__ volatile ("mrs %0, esr_el1; mrs %1, far_el1; mrs %2, elr_el1; mrs %3, spsr_el1"
                          : "=r"(esr), "=r"(far), "=r"(elr), "=r"(spsr));
    }
    uart_print("\r\n*** HUDOS MMU FAULT ***\r\n");
    uart_print_hex("CURRENT_EL=", el);
    uart_print_hex("ESR_ELx=", esr);
    uart_print_hex("FAR_ELx=", far);
    uart_print_hex("ELR_ELx=", elr);
    uart_print_hex("SPSR_ELx=", spsr);
    uart_print("Looping forever; reset to recover.\r\n");
    for (;;) __asm__ volatile ("wfi" ::: "memory");
}
void hudos_mmu_panic(u64 code) {
    uart_print("\r\n*** HUDOS UNHANDLED EXCEPTION (IRQ/FIQ/SERR) ***\r\n");
    uart_print_hex("CODE=", code);
    for (;;) __asm__ volatile ("wfi" ::: "memory");
}

/* ---- install and enable (Phase B/C) ---- */
extern char __hudos_vector_table[];
extern void hudos_set_vbar(u64 addr);
extern void hudos_drop_el1(void);
extern void hudos_enable_mmu(u64 l1, u64 tcr, u64 mair);

/* Clean the page-table region to the point of coherence so the MMU walker sees
 * our writes. */
static void hudos_dcache_clean(u64 start, u64 size) {
    u64 end = start + size;
    for (u64 a = start & ~(u64)63; a < end; a += 64)
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
}

/* ===========================================================================
 * hudos_mmu_init — Phase A entry point called from efi_main().
 * ========================================================================= */
void hudos_mmu_init(EFI_SYSTEM_TABLE *st, EFI_BOOT_SERVICES *bs) {
    /* Phase A — DEFERRED (boot-safe no-op).
     *
     * Intended goal: install an EL1 exception vector table so hudos can capture
     * ESR/FAR/ELR on a fault instead of dying silently.
     *
     * Why it is NOT done here (and why the previous attempt "didn't enter the
     * system"): hudos is a UEFI application that NEVER calls ExitBootServices, so
     * UEFI boot services (filesystem mount, GOP, block-IO, network, sound) stay
     * live for the whole OS lifetime. Those services are interrupt-driven and
     * rely on the firmware's own VBAR_EL1 handlers. Overriding VBAR_EL1 with our
     * vectors meant every IRQ taken during a boot-service call hit our stub
     * (which just `eret`s without acknowledging the interrupt); the pending IRQ
     * then re-fired instantly and the CPU spun in our IRQ vector forever — a
     * silent hang right after the banner (no sync fault, hence no
     * "HUDOS MMU FAULT" on the serial log).
     *
     * Therefore, while firmware boot services are alive we must keep BOTH the
     * firmware's MMU AND its vector table. The firmware already runs with the MMU
     * enabled and a 1:1 (VA==PA) map, which is all hudos needs today. Custom EL1
     * exception capture can only be installed safely AFTER ExitBootServices — a
     * future change. The identity-map page-table builder and vector-table code in
     * this file are retained as the foundation for that, and for Phase B/C (EL0
     * drop via a TTBR0 switch). */
    (void)st; (void)bs;
}
