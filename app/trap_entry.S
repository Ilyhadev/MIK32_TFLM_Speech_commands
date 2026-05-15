/* Trap vector at ROM+0xC0 (mtvec). Must live in .text.trap_handler per user.ld. */
#define EXCEPTION_STACK_SPACE 32*4
#define EXCEPTION_SAVED_REGISTERS 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31

.altmacro

.section .text.trap_handler, "ax", @progbits
.globl trap_entry
trap_entry:
    j       raw_trap_handler

.section .text, "ax", @progbits
.globl raw_trap_handler
raw_trap_handler:
    addi    sp, sp, -(EXCEPTION_STACK_SPACE)
    .irp index, EXCEPTION_SAVED_REGISTERS
        sw      x\index, 4*index(sp)
    .endr

    la      ra, app_trap_handler
    jalr    ra

    .irp index, EXCEPTION_SAVED_REGISTERS
        lw      x\index, 4*index(sp)
    .endr
    addi    sp, sp, EXCEPTION_STACK_SPACE
    mret
