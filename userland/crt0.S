/* hudos-server userland C runtime startup (aarch64).
 * The loader calls the ELF entry (_start) with:
 *   x0 = argc, x1 = argv, x2 = envp (NULL), x3 = syscall dispatch pointer,
 *   x4 = userland stack pointer (top, 16-byte aligned).
 * We save the loader's return address (lr) and stack pointer in
 * callee-saved registers, switch to the userland stack, store the syscall
 * pointer into __syscall, call main(), then restore the loader context and
 * return to the loader (via the blr in call_elf). main's return value
 * becomes the process exit code. */
    .section .text
    .globl _start
_start:
    mov x19, lr              /* save return address to loader */
    mov x20, sp              /* save loader stack pointer */
    mov sp, x4               /* switch to userland stack */
    adrp x9, __syscall
    str  x3, [x9, #:lo12:__syscall]   /* save syscall pointer */
    bl   main
    mov  sp, x20             /* restore loader stack pointer */
    mov  lr, x19             /* restore return address to loader */
    ret
    .end
