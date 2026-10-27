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
.rept 16
    b NativeException
    .balign 128
.endr

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
    b.ne NativeFatal
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
