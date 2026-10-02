/* hudos Phase A: EL1 exception vector table + MMU enable helper.
 *
 * C flow (mmu.c) calls:
 *   hudos_set_vbar(table)   - install vector table at the current EL
 *   hudos_drop_el1()        - if running at EL2, drop to EL1 (so the whole
 *                             OS is consistent and UEFI runtime services work)
 *   hudos_enable_mmu(l1,tcr,mair) - enable identity-map MMU at EL1
 *
 * All registers are clobbered; these are one-way transitions. */

    .text
    .align 11
    .globl __hudos_vector_table
__hudos_vector_table:
    /* ---- Current EL with SP0 ---- */
    b .                      /* 0x000 sync  */
    .balign 0x80
    b .                      /* 0x080 irq   */
    .balign 0x80
    b .                      /* 0x100 fiq   */
    .balign 0x80
    b .                      /* 0x180 serr  */
    /* ---- Current EL with SPx ---- */
    .balign 0x80
    b hudos_vec_sync         /* 0x200 sync  */
    .balign 0x80
    b hudos_vec_irq          /* 0x280 irq   */
    .balign 0x80
    b hudos_vec_fiq          /* 0x300 fiq   */
    .balign 0x80
    b hudos_vec_serr         /* 0x380 serr  */
    /* ---- Lower EL (AArch64) ---- */
    .balign 0x80
    b hudos_vec_sync         /* 0x400 sync  */
    .balign 0x80
    b hudos_vec_irq          /* 0x480 irq   */
    .balign 0x80
    b hudos_vec_fiq          /* 0x500 fiq   */
    .balign 0x80
    b hudos_vec_serr         /* 0x580 serr  */
    /* ---- Lower EL (AArch32) ---- */
    .balign 0x80
    b .                      /* 0x600 sync  */
    .balign 0x80
    b .                      /* 0x680 irq   */
    .balign 0x80
    b .                      /* 0x700 fiq   */
    .balign 0x80
    b .                      /* 0x780 serr  */

/* ---- synchronous fault: save regs, call C fault handler ---- */
hudos_vec_sync:
    /* raw UART 'F' so we know a synchronous fault reached the handler
     * even if the C path (registers/stack) is broken. */
    mov  x0, #0x9000000
1:  ldr  w1, [x0, #0x18]
    tbnz w1, #5, 1b
    mov  w1, #0x46            /* 'F' */
    str  w1, [x0]
    sub  sp, sp, #0x100
    stp  x0,  x1,  [sp, #0x00]
    stp  x2,  x3,  [sp, #0x10]
    stp  x4,  x5,  [sp, #0x20]
    stp  x6,  x7,  [sp, #0x30]
    stp  x8,  x9,  [sp, #0x40]
    stp  x10, x11, [sp, #0x50]
    stp  x12, x13, [sp, #0x60]
    stp  x14, x15, [sp, #0x70]
    stp  x16, x17, [sp, #0x80]
    stp  x18, x19, [sp, #0x90]
    stp  x20, x21, [sp, #0xA0]
    stp  x22, x23, [sp, #0xB0]
    stp  x24, x25, [sp, #0xC0]
    stp  x26, x27, [sp, #0xD0]
    stp  x28, x29, [sp, #0xE0]
    stp  x30, xzr, [sp, #0xF0]
    bl   hudos_mmu_fault_entry
1:  b    1b                  /* does not return */

hudos_vec_irq:
hudos_vec_fiq:
hudos_vec_serr:
    /* We do not use interrupts in Phase A; just return from the exception. */
    eret

/* ---- install vector base at the current EL ---- */
    .globl hudos_set_vbar
hudos_set_vbar:
    mrs  x1, currentel
    tbz  x1, 3, 1f          /* EL1 (0x4) -> bit3 clear -> EL1 path */
    msr  vbar_el2, x0
    ret
1:  msr  vbar_el1, x0
    ret

/* ---- drop from EL2 to EL1 if necessary ----
 * Clobbers x0-x4. Returns (via eret) to the caller at EL1 when we were at EL2. */
    .globl hudos_drop_el1
hudos_drop_el1:
    mrs  x0, currentel
    tbz  x0, 3, 1f          /* already EL1 -> just return */
    /* EL2: configure EL1 to be AArch64 and hand over. */
    mov  x0, #(1 << 31)     /* HCR_EL2.RW = 1 (EL1 is AArch64) */
    msr  hcr_el2, x0
    mov  x0, #0x5           /* SPSR_EL1: D/A/I/F masked, M[3:0]=0x5 (EL1h) */
    msr  spsr_el1, x0
    mov  x0, x30            /* ELR_EL1 = return address into the caller */
    msr  elr_el1, x0
    mov  x0, sp             /* reuse current stack for EL1 */
    msr  sp_el1, x0
    eret                    /* -> EL1, continues at the caller */
1:  ret

/* ---- enable identity-map MMU at EL1 ----
 * x0 = L0 page table PA, x1 = TCR_EL1, x2 = MAIR_EL1
 *
 * The UEFI firmware hands us control with the MMU ALREADY enabled (M=1) and its
 * own TTBR0/TCR/MAIR. We take it over WITHOUT turning the MMU off: TTBR0/TCR/
 * MAIR are written back-to-back and committed with a single isb. Because no
 * instruction fetch happens between the msr writes and the isb, the first fetch
 * after the isb uses our NEW TCR + NEW TTBR0 together -- there is no window where
 * the old TCR walks our tables (which would fault). This avoids the M=0/M=1
 * toggle, which proved unreliable under QEMU's soft-MMU (the post-enable fetch
 * faulted as a bogus "permission fault level 2" even though the tables were
 * provably correct). */
    .globl hudos_enable_mmu
hudos_enable_mmu:
    msr  daifset, #0x3        /* mask IRQ/FIQ before disturbing the regime */
    /* Install our page tables + translation regime (no fetch between these). */
    msr  ttbr0_el1, x0
    msr  ttbr1_el1, x0
    msr  mair_el1, x2
    msr  tcr_el1,  x1
    tlbi vmalle1
    dsb  ish
    /* MARK 'T' = page tables + regime installed (MMU still on; UEFI tables in
     * effect only until the isb below commits our new regime). */
    mov  x4, #0x9000000
2:  ldr  w1, [x4, #0x18]
    tbnz w1, #5, 2b
    mov  w1, #0x54
    str  w1, [x4]
    /* Re-commit SCTLR with caches on, MMU on, WXN/UWXN cleared. M was already 1. */
    mrs  x3, sctlr_el1
    orr  x3, x3, #(1 << 2)   /* C  enable data cache */
    orr  x3, x3, #(1 << 12)  /* I  enable instr cache*/
    /* EDK2/UEFI may boot with SCTLR_EL1.WXN=1 / UWXN=1. Our flat OS maps the
     * loaded image (code AND data) as EL1 read-write (AP=0b01). With WXN set,
     * an RW page is treated as XN for unprivileged (and the legacy XN bit also
     * gates EL1 execute on some cores); clearing WXN/UWXN keeps RW pages
     * executable under the new regime. */
    bic  x3, x3, #(1 << 19)  /* clear WXN  */
    bic  x3, x3, #(1 << 20)  /* clear UWXN */
    msr  sctlr_el1, x3
    isb                       /* commit; first fetch now uses OUR tables + TCR */
    /* MARK 'M' = our MMU regime active; if we reach here the post-commit fetch
     * succeeded through our identity map. */
    mov  x4, #0x9000000
3:  ldr  w1, [x4, #0x18]
    tbnz w1, #5, 3b
    mov  w1, #0x4D
    str  w1, [x4]
    /* Flush I-cache so we do not execute stale lines fetched under UEFI. */
    ic   iallu
    dsb  ish
    isb
    /* raw UART 'A' = MMU take-over succeeded and we reached post-enable code. */
    mov  x4, #0x9000000
    mov  w5, #0x41            /* 'A' */
    str  w5, [x4]
    ret

/* ---- diagnostic UART helpers (called with MMU OFF so physical IO works) ---- */
dbg_putc:
    mov  x4, #0x9000000
3:  ldr  w5, [x4, #0x18]
    tbnz w5, #5, 3b
    str  w0, [x4]
    ret

dbg_hex:
    stp  x29, x30, [sp, #-16]!
    stp  x1,  x2,  [sp, #-16]!
    stp  x3,  x4,  [sp, #-16]!
    mov  x1, x0
    mov  x2, #60
1:  lsr  x0, x1, x2
    and  x0, x0, #0xF
    cmp  x0, #10
    mov  x3, #0x30
    add  x0, x0, x3
    bl   dbg_putc
    sub  x2, x2, #4
    cmp  x2, #0
    bge  1b
    mov  x0, #0x20            /* space */
    bl   dbg_putc
    ldp  x3,  x4,  [sp], #16
    ldp  x1,  x2,  [sp], #16
    ldp  x29, x30, [sp], #16
    ret

/* Phase B scratch state (in .data: no cross-object relocations needed). */
    .data
    .balign 16
    .globl hudos_el0_kctx
hudos_el0_kctx:
    .space 128
    .globl hudos_el0_stats
hudos_el0_stats:
    .space 64

    .text
    .align 11
    .globl hudos_el0_vectors
hudos_el0_vectors:
    b    hudos_el1_unexp   /* 0x000 sync  EL1t */
    .space 124
    b    hudos_el1_unexp   /* 0x080 irq   EL1t */
    .space 124
    b    hudos_el1_unexp   /* 0x100 fiq   EL1t */
    .space 124
    b    hudos_el1_unexp   /* 0x180 serr  EL1t */
    .space 124
    b    hudos_el1_unexp   /* 0x200 sync  EL1h */
    .space 124
    b    hudos_el1_unexp   /* 0x280 irq   EL1h */
    .space 124
    b    hudos_el1_unexp   /* 0x300 fiq   EL1h */
    .space 124
    b    hudos_el1_unexp   /* 0x380 serr  EL1h */
    .space 124
    b    hudos_el0_sync    /* 0x400 sync  EL0  <-- SVC from EL0 lands here */
    .space 124
    b    hudos_el0_async_irq  /* 0x480 irq   EL0 */
    .space 124
    b    hudos_el0_async_fiq  /* 0x500 fiq   EL0 */
    .space 124
    b    hudos_el0_async_serr /* 0x580 serr  EL0 */
    .space 124
    b    hudos_el1_unexp   /* 0x600 sync  EL0 AArch32 */
    .space 124
    b    hudos_el1_unexp   /* 0x680 irq   EL0 AArch32 */
    .space 124
    b    hudos_el1_unexp   /* 0x700 fiq   EL0 AArch32 */
    .space 124
    b    hudos_el1_unexp   /* 0x780 serr  EL0 AArch32 */
    .space 124
    /* End of the 2048-byte table. Every slot above is exactly 128 bytes; getting
     * this wrong silently shifts every later slot, so handler code must live
     * strictly BEYOND this point (verified with llvm-nm: each handler label is
     * at least 0x800 past hudos_el0_vectors). */

/* Async slots share hudos_el0_async but tag which vector they came from.
 * These stubs sit outside the table so they cannot disturb its layout. */
hudos_el0_async_irq:
    mov  x9, #1
    b    hudos_el0_async
hudos_el0_async_fiq:
    mov  x9, #2
    b    hudos_el0_async
hudos_el0_async_serr:
    mov  x9, #3
    b    hudos_el0_async

/* Save x0-x30 into a 256-byte frame on the current stack, then hand the frame
 * pointer to the C handler. Shared by all three entry points below. */
hudos_el0_sync:
    sub  sp, sp, #0x100
    stp  x0,  x1,  [sp, #0x00]
    stp  x2,  x3,  [sp, #0x10]
    stp  x4,  x5,  [sp, #0x20]
    stp  x6,  x7,  [sp, #0x30]
    stp  x8,  x9,  [sp, #0x40]
    stp  x10, x11, [sp, #0x50]
    stp  x12, x13, [sp, #0x60]
    stp  x14, x15, [sp, #0x70]
    stp  x16, x17, [sp, #0x80]
    stp  x18, x19, [sp, #0x90]
    stp  x20, x21, [sp, #0xA0]
    stp  x22, x23, [sp, #0xB0]
    stp  x24, x25, [sp, #0xC0]
    stp  x26, x27, [sp, #0xD0]
    stp  x28, x29, [sp, #0xE0]
    stp  x30, xzr, [sp, #0xF0]
    mov  x0,  sp
    bl   hudos_el0_sync_handler
    b    .

hudos_el0_async:
    sub  sp, sp, #0x100
    stp  x0,  x1,  [sp, #0x00]
    stp  x2,  x3,  [sp, #0x10]
    stp  x4,  x5,  [sp, #0x20]
    stp  x6,  x7,  [sp, #0x30]
    stp  x8,  x9,  [sp, #0x40]
    stp  x10, x11, [sp, #0x50]
    stp  x12, x13, [sp, #0x60]
    stp  x14, x15, [sp, #0x70]
    stp  x16, x17, [sp, #0x80]
    stp  x18, x19, [sp, #0x90]
    stp  x20, x21, [sp, #0xA0]
    stp  x22, x23, [sp, #0xB0]
    stp  x24, x25, [sp, #0xC0]
    stp  x26, x27, [sp, #0xD0]
    stp  x28, x29, [sp, #0xE0]
    stp  x30, xzr, [sp, #0xF0]
    str  x9,       [sp, #0xF8]   /* lower-EL slot tag set at 0x480/0x500/0x580 */
    mov  x0,  sp
    bl   hudos_el0_async_handler
    b    .

hudos_el1_unexp:
    sub  sp, sp, #0x100
    stp  x0,  x1,  [sp, #0x00]
    stp  x2,  x3,  [sp, #0x10]
    stp  x4,  x5,  [sp, #0x20]
    stp  x6,  x7,  [sp, #0x30]
    stp  x8,  x9,  [sp, #0x40]
    stp  x10, x11, [sp, #0x50]
    stp  x12, x13, [sp, #0x60]
    stp  x14, x15, [sp, #0x70]
    stp  x16, x17, [sp, #0x80]
    stp  x18, x19, [sp, #0x90]
    stp  x20, x21, [sp, #0xA0]
    stp  x22, x23, [sp, #0xB0]
    stp  x24, x25, [sp, #0xC0]
    stp  x26, x27, [sp, #0xD0]
    stp  x28, x29, [sp, #0xE0]
    stp  x30, xzr, [sp, #0xF0]
    mov  x0,  sp
    bl   hudos_el1_unexp_handler
    b    .

/* Resume the EL0 program.
 * x0 = frame pointer. x0/x1 must be reloaded LAST because x0 is the base. */
    .globl hudos_el0_eret
hudos_el0_eret:
    /* Undo the 0x100-byte frame the entry stub pushed, otherwise every syscall
     * would leak 256 bytes of kernel stack and eventually overflow it.
     * x9 is safe to clobber here: it is reloaded from the frame below. */
    add  x9,  x0,  #0x100
    mov  sp,  x9
    ldp  x2,  x3,  [x0, #0x10]
    ldp  x4,  x5,  [x0, #0x20]
    ldp  x6,  x7,  [x0, #0x30]
    ldp  x8,  x9,  [x0, #0x40]
    ldp  x10, x11, [x0, #0x50]
    ldp  x12, x13, [x0, #0x60]
    ldp  x14, x15, [x0, #0x70]
    ldp  x16, x17, [x0, #0x80]
    ldp  x18, x19, [x0, #0x90]
    ldp  x20, x21, [x0, #0xA0]
    ldp  x22, x23, [x0, #0xB0]
    ldp  x24, x25, [x0, #0xC0]
    ldp  x26, x27, [x0, #0xD0]
    ldp  x28, x29, [x0, #0xE0]
    ldr  x30,      [x0, #0xF0]
    ldp  x0,  x1,  [x0, #0x00]
    eret

/* Resume an EL0 program from a full execution context (registers + elr + spsr +
 * sp_el0). Unlike hudos_el0_eret (which only reloads x0..x30 and relies on the
 * CPU's current elr/spsr), this restores elr_el1/spsr_el1/sp_el0 from the context
 * so the privilege level and return address are correct after a coroutine yield.
 *   x0 = struct el0_ctx*  (x[0..30] @0, elr @0x100, spsr @0x108, sp_el0 @0x110)
 * All x0..x30 are reloaded from the frame; elr/spsr/sp_el0 are stashed on the
 * kernel stack first (since every GPR also lives in the frame and gets clobbered
 * during reload) and restored just before eret. */
    .globl hudos_el0_ctx_eret
hudos_el0_ctx_eret:
    sub  sp,  sp, #0x20
    ldr  x9,  [x0, #0x100]             /* elr_el1  */
    ldr  x10, [x0, #0x108]             /* spsr_el1 */
    ldr  x11, [x0, #0x110]             /* sp_el0   */
    stp  x9,  x10, [sp, #0x00]
    str  x11,      [sp, #0x10]
    /* reload GPRs x0..x30 (x0,x1 reloaded last) */
    ldp  x2,  x3,  [x0, #0x10]
    ldp  x4,  x5,  [x0, #0x20]
    ldp  x6,  x7,  [x0, #0x30]
    ldp  x8,  x9,  [x0, #0x40]
    ldp  x10, x11, [x0, #0x50]
    ldp  x12, x13, [x0, #0x60]
    ldp  x14, x15, [x0, #0x70]
    ldp  x16, x17, [x0, #0x80]
    ldp  x18, x19, [x0, #0x90]
    ldp  x20, x21, [x0, #0xA0]
    ldp  x22, x23, [x0, #0xB0]
    ldp  x24, x25, [x0, #0xC0]
    ldp  x26, x27, [x0, #0xD0]
    ldp  x28, x29, [x0, #0xE0]
    ldr  x30,      [x0, #0xF0]
    ldp  x0,  x1,  [x0, #0x00]
    /* restore special registers from the stashed copy */
    ldp  x9,  x10, [sp, #0x00]
    ldr  x11,      [sp, #0x10]
    add  sp,  sp,  #0x20
    msr  elr_el1,  x9
    msr  spsr_el1, x10
    msr  sp_el0,   x11
    eret

/* Terminate the EL0 program: restore the kernel context captured by
 * hudos_el0_enter and return to it, with x0 already holding the exit code. */
    .globl hudos_el0_exit
hudos_el0_exit:
    adrp x9,  hudos_el0_kctx
    add  x9,  x9,  #:lo12:hudos_el0_kctx
    ldp  x19, x20, [x9, #0x00]
    ldp  x21, x22, [x9, #0x10]
    ldp  x23, x24, [x9, #0x20]
    ldp  x25, x26, [x9, #0x30]
    ldp  x27, x28, [x9, #0x40]
    ldp  x29, x30, [x9, #0x50]
    ldr  x10,      [x9, #0x60]
    mov  sp,  x10
    isb
    ret

/* Drop into EL0 and start running the loaded program.
 * x0 = entry point, x1 = user stack top (SP_EL0),
 * x2 = argc, x3 = argv, x4 = envp.
 * Returns (by way of hudos_el0_exit) to the caller with x0 = exit code. */
    .globl hudos_el0_enter
hudos_el0_enter:
    adrp x9,  hudos_el0_kctx
    add  x9,  x9,  #:lo12:hudos_el0_kctx
    stp  x19, x20, [x9, #0x00]
    stp  x21, x22, [x9, #0x10]
    stp  x23, x24, [x9, #0x20]
    stp  x25, x26, [x9, #0x30]
    stp  x27, x28, [x9, #0x40]
    stp  x29, x30, [x9, #0x50]
    mov  x10, sp
    str  x10,      [x9, #0x60]
    /* stash the EL0 arguments in callee-saved regs while we reprogram others */
    mov  x19, x0              /* entry  */
    mov  x20, x2              /* argc   */
    mov  x21, x3              /* argv   */
    mov  x22, x4              /* envp   */
    mov  x24, x1              /* keep the user stack top for x4 below */
    msr  sp_el0, x1           /* user stack */
    msr  elr_el1, x19         /* resume at the ELF entry */
    mov  x23, #0x3c0          /* PSTATE: D,A,I,F masked; M[3:0]=0 -> EL0t */
    msr  spsr_el1, x23
    mov  x0,  x20             /* argc */
    mov  x1,  x21             /* argv */
    mov  x2,  x22             /* envp */
    mov  x3,  xzr             /* x3 = 0 tells crt0 to use the SVC ABI */
    mov  x4,  x24             /* x4 = user stack top (legacy crt0 convention) */
    dsb  ish
    isb
    eret

/* First entry into a coroutine child running at EL0.
 * Unlike the direct-exec case we must also capture the PARENT's kernel context
 * (x19-x30 + sp) so the child's first yield can hand control back to the GUI,
 * and we must switch SP to a kernel stack owned by this child: its syscalls run
 * there, otherwise they would share (and corrupt) the GUI's stack.
 *   x0 = ELF entry, x1 = user stack (SP_EL0), x2 = child kernel stack top,
 *   x3 = argc, x4 = argv, x5 = &g_co_parent */
    .globl hudos_el0_co_start
hudos_el0_co_start:
    mov  x6,  x1               /* keep the user stack */
    mov  x7,  sp
    str  x7,  [x5, #96]        /* parent sp */
    stp  x19, x20, [x5, #0]
    stp  x21, x22, [x5, #16]
    stp  x23, x24, [x5, #32]
    stp  x25, x26, [x5, #48]
    stp  x27, x28, [x5, #64]
    stp  x29, x30, [x5, #80]
    mov  sp,  x2               /* this child's own kernel stack */
    msr  sp_el0, x6            /* user stack */
    msr  elr_el1, x0           /* resume at the ELF entry */
    mov  x7,  #0x3c0           /* EL0t, D/A/I/F masked */
    msr  spsr_el1, x7
    mov  x0,  x3               /* argc */
    mov  x1,  x4               /* argv */
    mov  x2,  xzr              /* envp */
    mov  x3,  xzr              /* 0 => crt0 uses the SVC ABI */
    mov  x4,  x6               /* user stack top (legacy crt0 convention) */
    /* Phase B: install the child's private TTBR0 (child ASID + cloned root) so the
     * EL0 view contains ONLY the child image. g_el0_child_ttbr0 equals the firmware
     * TTBR0 when Phase B is off, making this a harmless no-op in that case. */
    adrp x8, g_el0_child_ttbr0
    add  x8,  x8,  #:lo12:g_el0_child_ttbr0
    ldr  x8,  [x8]
    msr  ttbr0_el1, x8
    dsb  ish
    tlbi vmalle1is
    dsb  ish
    isb
    eret

/* Boot self-test target: entered at EL0, immediately traps back with n=999. */
    .globl hudos_el0_selftest_entry
hudos_el0_selftest_entry:
    mov  x8,  #999
    svc  #0
    b    .
