.section .text.boot, "ax"
.global _start
_start:
    msr daifset, #15
    mrs x0, mpidr_el1
    and x0, x0, #0xffffff
    cbnz x0, parked_secondary
    ldr x1, =stack_top
    mov sp, x1
    mrs x0, CurrentEL
    cmp x0, #12
    b.eq from_el3
    cmp x0, #8
    b.eq from_el2
    b at_el1
from_el3:
    ldr x2, =0x08000080
    mov w3, #-1
    mov x4, #32
1:
    str w3, [x2], #4
    subs x4, x4, #1
    b.ne 1b
    ldr x2, =0x08000084
    mov w3, #0xfffffdff
    str w3, [x2]
#ifndef GIC_TEST_BLOCKED_PMR
    ldr x2, =0x08010004
    mov w3, #0xff
    str w3, [x2]
#endif
    mov x0, #0x501
    msr scr_el3, x0
    msr cptr_el3, xzr
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    msr cptr_el2, xzr
    msr sp_el1, x1
    mov x0, #0x3c5
    msr spsr_el3, x0
    adr x0, at_el1
    msr elr_el3, x0
    dsb sy
    isb
    eret
from_el2:
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    msr cptr_el2, xzr
    msr sp_el1, x1
    mov x0, #0x3c5
    msr spsr_el2, x0
    adr x0, at_el1
    msr elr_el2, x0
    isb
    eret
at_el1:
    msr spsel, #1
    ldr x0, =vectors
    msr vbar_el1, x0
    msr cntv_ctl_el0, xzr
    msr cntp_ctl_el0, xzr
    isb
    ldr x0, =__bss_start
    ldr x1, =__bss_end
2:
    cmp x0, x1
    b.hs 3f
    str xzr, [x0], #8
    b 2b
3:
    bl gic_test_main
    b .

.macro VECTOR handler
    b \handler
    .balign 128
.endm
.balign 2048
vectors:
    VECTOR failed_vector
    VECTOR irq_vector
    VECTOR failed_vector
    VECTOR failed_vector
    VECTOR failed_vector
    VECTOR irq_vector
    VECTOR failed_vector
    VECTOR failed_vector
    VECTOR failed_vector
    VECTOR irq_vector
    VECTOR failed_vector
    VECTOR failed_vector
    VECTOR failed_vector
    VECTOR failed_vector
    VECTOR failed_vector
    VECTOR failed_vector
irq_vector:
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    bl gic_test_irq
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256
    eret
failed_vector:
    bl gic_test_exception
    b .
parked_secondary:
    wfe
    b parked_secondary

.section .bss.stack, "aw", %nobits
.balign 16
    .space 16384
stack_top:
