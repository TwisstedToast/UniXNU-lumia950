.text
.p2align 2
.globl NativeGuardedRead
NativeGuardedRead:
    mov x2, x0
    mov x0, #0
    dsb sy
NativeReadFaultSite:
    ldr w3, [x2]
    dsb sy
    str w3, [x1]
    mov x0, #1
NativeReadResume:
    ret

.globl NativeGuardedWrite
NativeGuardedWrite:
    mov x2, x0
    mov x0, #0
    dsb sy
NativeWriteFaultSite:
    str w1, [x2]
    dsb sy
    mov x0, #1
NativeWriteResume:
    ret

.globl NativeGuardedScmCall
NativeGuardedScmCall:
    stp x19, x20, [sp, #-32]!
    stp x29, x30, [sp, #16]
    mov x19, x0
    mov x20, #0
    ldp x0, x1, [x19]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
    dsb sy
NativeScmFaultSite:
    smc #0
    dsb sy
    stp x0, x1, [x19]
    stp x2, x3, [x19, #16]
    stp x4, x5, [x19, #32]
    stp x6, x7, [x19, #48]
    mov x20, #1
NativeScmResume:
    mov x0, x20
    ldp x29, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret

.globl NativeInstallVectors
NativeInstallVectors:
    mrs x0, vbar_el1
    adrp x1, NativeVectors
    add x1, x1, :lo12:NativeVectors
    msr vbar_el1, x1
    isb
    ret

.p2align 11
NativeVectors:
.macro NativeVector target
    b \target
    .balign 128
.endm
    NativeVector NativeException
    NativeVector NativeIrq
    NativeVector NativeException
    NativeVector NativeException
    NativeVector NativeException
    NativeVector NativeIrq
    NativeVector NativeException
    NativeVector NativeException
    NativeVector NativeException
    NativeVector NativeIrq
    NativeVector NativeException
    NativeVector NativeException
    NativeVector NativeException
    NativeVector NativeIrq
    NativeVector NativeException
    NativeVector NativeException

NativeIrq:
    sub sp, sp, #288
    stp x0, x1, [sp]
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
    mrs x9, elr_el1
    mrs x10, spsr_el1
    stp x9, x10, [sp, #256]
    bl NativeIrqDispatch
    cbnz w0, NativeIrqRestore
    ldr x9, [sp, #264]
    orr x9, x9, #0x80
    str x9, [sp, #264]
NativeIrqRestore:
    ldp x9, x10, [sp, #256]
    msr elr_el1, x9
    msr spsr_el1, x10
    ldp x0, x1, [sp]
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
    add sp, sp, #288
    eret

NativeException:
    stp x9, x10, [sp, #-32]!
    stp x11, x12, [sp, #16]
    mrs x9, esr_el1
    adrp x10, native_mmio_esr
    str x9, [x10, :lo12:native_mmio_esr]
    mrs x11, far_el1
    adrp x10, native_mmio_far
    str x11, [x10, :lo12:native_mmio_far]
    mrs x11, elr_el1
    adrp x10, native_mmio_elr
    str x11, [x10, :lo12:native_mmio_elr]
    lsr x9, x9, #26
    cmp x9, #0x25
    b.ne NativeCheckScmFault
    adrp x10, NativeReadFaultSite
    add x10, x10, :lo12:NativeReadFaultSite
    adrp x12, NativeReadResume
    add x12, x12, :lo12:NativeReadResume
    cmp x11, x10
    b.eq NativeResume
    adrp x10, NativeWriteFaultSite
    add x10, x10, :lo12:NativeWriteFaultSite
    adrp x12, NativeWriteResume
    add x12, x12, :lo12:NativeWriteResume
    cmp x11, x10
    b.ne NativeFatal
    b NativeResume
NativeCheckScmFault:
    cbnz x9, NativeFatal
    adrp x10, NativeScmFaultSite
    add x10, x10, :lo12:NativeScmFaultSite
    cmp x11, x10
    b.ne NativeFatal
    adrp x12, NativeScmResume
    add x12, x12, :lo12:NativeScmResume
NativeResume:
    msr elr_el1, x12
    ldp x11, x12, [sp, #16]
    ldp x9, x10, [sp], #32
    eret
NativeFatal:
    ldp x11, x12, [sp, #16]
    ldp x9, x10, [sp], #32
    b NativeFatalFault

.data
.p2align 3
.globl native_mmio_esr
native_mmio_esr: .quad 0
.globl native_mmio_far
native_mmio_far: .quad 0
.globl native_mmio_elr
native_mmio_elr: .quad 0
