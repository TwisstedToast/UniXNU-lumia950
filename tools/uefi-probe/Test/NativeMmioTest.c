#include <Uefi.h>

extern BOOLEAN NativeGuardedRead(UINT64 address, UINT32 *value);
extern BOOLEAN NativeGuardedWrite(UINT64 address, UINT32 value);
extern BOOLEAN NativeGuardedScmCall(UINT64 *registers);
extern UINT64 NativeInstallVectors(VOID);
extern volatile UINT64 native_mmio_esr, native_mmio_far, native_mmio_elr;

VOID NativeFatalFault(VOID)
{
    for (;;) { __asm__ volatile("wfi"); }
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *firmware)
{
    (void)image;
    UINT64 savedDaif = 0;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15" : "=r"(savedDaif) :: "memory");
    UINT64 previous = NativeInstallVectors();
    volatile UINT32 storage = 0x418;
    UINT32 readback = 0;
    const UINT64 invalid = 0xffff000000000000ull;
    BOOLEAN passed = NativeGuardedRead((UINT64)(UINTN)&storage, &readback) && readback == 0x418;
    passed = passed && NativeGuardedWrite((UINT64)(UINTN)&storage, 0x8992) && storage == 0x8992;
    readback = 0x12345678;
    passed = passed && !NativeGuardedRead(invalid, &readback) && readback == 0x12345678 &&
        (native_mmio_esr >> 26) == 0x25 && native_mmio_far == invalid;
    passed = passed && !NativeGuardedWrite(invalid, 0x8992) &&
        (native_mmio_esr >> 26) == 0x25 && native_mmio_far == invalid;
    passed = passed && NativeGuardedRead((UINT64)(UINTN)&storage, &readback) && readback == 0x8992;
    UINT64 registers[8] = {0x02000601, 1, 0x02000c02, 0, 0, 0, 0, 0};
    BOOLEAN returned = NativeGuardedScmCall(registers);
    passed = passed && ((returned && (registers[0] == 0xffffffffu || registers[0] == ~0ull)) ||
        (!returned && (native_mmio_esr >> 26) == 0));
    passed = passed && NativeGuardedRead((UINT64)(UINTN)&storage, &readback) && readback == 0x8992;
    __asm__ volatile("msr vbar_el1, %0\nisb\nmsr daif, %1" :: "r"(previous), "r"(savedDaif) : "memory");
    firmware->ConOut->OutputString(firmware->ConOut,
        passed ? L"NATIVE MMIO GUARD PASSED\r\n" : L"NATIVE MMIO GUARD FAILED\r\n");
    firmware->ConOut->OutputString(firmware->ConOut,
        passed ? L"NATIVE SCM GUARD PASSED\r\n" : L"NATIVE SCM GUARD FAILED\r\n");
    if (passed) {
        firmware->ConOut->OutputString(firmware->ConOut,
            returned ? L"SCM_QUERY_RETURNED_UNSUPPORTED\r\n" : L"SCM_UNDEFINED_TRAP_RECOVERED\r\n");
    }
    return passed ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
