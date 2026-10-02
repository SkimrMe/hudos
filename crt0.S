/* hudos-server userland C runtime startup (aarch64).
 * The loader calls the ELF entry (_start) with:
 *   x0 = argc, x1 = argv, x2 = envp (NULL), x3 = syscall dispatch pointer,
 *   x4 = userland stack pointer (top, 16-byte aligned).
 * We switch to the userland stack, store the syscall pointer into __syscall,
 * call main(), then restore the loader's stack pointer and return address and
 * return to the loader (via the blr in call_elf). main's return value becomes
 * the process exit code.
 *
 * IMPORTANT: we must NOT clobber any callee-saved register (x19-x28) that the
 * loader relies on, and we must restore the loader's stack pointer before
 * returning. We save the loader's lr (x30) and sp on the userland stack. */
    .section .text
    .globl _start
_start:
    mov x9, sp               /* save loader stack pointer temporarily */
    mov sp, x4               /* switch to the userland stack */
    sub sp, sp, #32
    str x30, [sp]            /* save loader return address (lr) */
    str x9,  [sp, #8]        /* save loader stack pointer */
    str x3,  [sp, #16]       /* remember the mode flag (main clobbers x3) */
    adrp x9, __syscall
    str  x3, [x9, #:lo12:__syscall]   /* save syscall dispatch pointer */
    bl   main
    ldr x3,  [sp, #16]
    cbz x3,  el0_exit        /* 0 => started at EL0, leave via SVC */
    ldr x30, [sp]            /* restore loader return address */
    ldr x9,  [sp, #8]        /* restore loader stack pointer */
    mov sp, x9
    ret
el0_exit:
    /* Running unprivileged: `ret` cannot cross the EL0 -> EL1 boundary, so ask
     * the kernel to terminate us. x0 already holds main's return value. */
    mov x8, #93              /* SYS_exit */
    svc #0
    b   .                    /* not reached */
    .end
