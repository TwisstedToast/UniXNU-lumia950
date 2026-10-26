#include <Uefi.h>
#include <Guid/Acpi.h>

#include "BoardConstants.h"
#include "GpuIo.h"
#include "PlatformCheck.h"
#include "ReportStore.h"
#include "FirmwareClock.h"

extern "C" {
#include "ProbeCore.h"
}

static EFI_SYSTEM_TABLE *system_table;
alignas(64) static char report_buffer[8192];
static UINTN report_bytes;
static bool report_overflow;

extern "C" void *memset(void *destination, int value, size_t bytes)
{
    unsigned char *output = static_cast<unsigned char *>(destination);
    for (size_t index = 0; index < bytes; index++) { output[index] = static_cast<unsigned char>(value); }
    return destination;
}

extern "C" void *memcpy(void *destination, const void *source, size_t bytes)
{
    unsigned char *output = static_cast<unsigned char *>(destination);
    const unsigned char *input = static_cast<const unsigned char *>(source);
    for (size_t index = 0; index < bytes; index++) { output[index] = input[index]; }
    return destination;
}

static void Print(const char *text)
{
    CHAR16 console[96];
    UINTN used = 0;
    while (*text) {
        const char value = *text++;
        if (report_bytes < sizeof(report_buffer)) { report_buffer[report_bytes++] = value; }
        else { report_overflow = true; }
        console[used++] = static_cast<unsigned char>(value);
        if (used == 95 || *text == 0) {
            console[used] = 0;
            system_table->ConOut->OutputString(system_table->ConOut, console);
            used = 0;
        }
    }
}

static void PrintHex(const char *label, UINT64 value)
{
    char digits[65];
    Print(label); Print("=0x");
    if (ProbeFormatUnsigned(value, 16, 16, digits, sizeof(digits))) { Print(digits); }
    Print("\r\n");
}

static bool SameGuid(const EFI_GUID &first, const EFI_GUID &second)
{
    const UINT8 *first_bytes = reinterpret_cast<const UINT8 *>(&first);
    const UINT8 *second_bytes = reinterpret_cast<const UINT8 *>(&second);
    for (UINTN index = 0; index < sizeof(EFI_GUID); index++) {
        if (first_bytes[index] != second_bytes[index]) { return false; }
    }
    return true;
}

static UINT32 Read32(const UINT8 *value)
{
    return value[0] | (static_cast<UINT32>(value[1]) << 8) |
        (static_cast<UINT32>(value[2]) << 16) | (static_cast<UINT32>(value[3]) << 24);
}

static UINT64 Read64(const UINT8 *value)
{
    return Read32(value) | (static_cast<UINT64>(Read32(value + 4)) << 32);
}

static bool Checksum(const UINT8 *bytes, UINTN count)
{
    UINT8 sum = 0;
    for (UINTN index = 0; index < count; index++) { sum = static_cast<UINT8>(sum + bytes[index]); }
    return sum == 0;
}

static bool AcpiMatches()
{
    const EFI_GUID acpi_guid = EFI_ACPI_20_TABLE_GUID;
    const UINT8 *rsdp = nullptr;
    for (UINTN index = 0; index < system_table->NumberOfTableEntries; index++) {
        if (SameGuid(system_table->ConfigurationTable[index].VendorGuid, acpi_guid)) {
            rsdp = static_cast<const UINT8 *>(system_table->ConfigurationTable[index].VendorTable);
            break;
        }
    }
    if (!rsdp || Read64(rsdp) != 0x2052545020445352ull || rsdp[15] < 2 ||
        !Checksum(rsdp, 20)) { return false; }
    const UINT32 rsdp_bytes = Read32(rsdp + 20);
    if (rsdp_bytes < 36 || rsdp_bytes > 4096 || !Checksum(rsdp, rsdp_bytes)) { return false; }
    const UINT64 xsdt_address = Read64(rsdp + 24);
    if (!xsdt_address || (xsdt_address & 3)) { return false; }
    const UINT8 *xsdt = reinterpret_cast<const UINT8 *>(xsdt_address);
    const UINT32 xsdt_bytes = Read32(xsdt + 4);
    if (Read32(xsdt) != 0x54445358 || xsdt_bytes < 36 || xsdt_bytes > 65536 ||
        (xsdt_bytes - 36) % 8 != 0 || !Checksum(xsdt, xsdt_bytes)) { return false; }
    for (UINT32 offset = 36; offset < xsdt_bytes; offset += 8) {
        const UINT64 address = Read64(xsdt + offset);
        if (!address || (address & 3)) { continue; }
        const UINT8 *table = reinterpret_cast<const UINT8 *>(address);
        if (Read32(table) == 0x43495041) {
            return adreno::isLumia950Madt(table, Read32(table + 4));
        }
    }
    return false;
}

static bool MmioRead(void *, adreno::RegisterSpace space, adreno::u32 offset,
                     adreno::u32 *value)
{
    UINT64 base = 0;
    UINT32 bytes = 0;
    switch (space) {
    case adreno::RegisterSpace::Mmss: base = lumia950::kMmssPhys; bytes = lumia950::kMmssSize; break;
    case adreno::RegisterSpace::Gpu: base = lumia950::kGpuPhys; bytes = lumia950::kGpuSize; break;
    case adreno::RegisterSpace::Smmu: base = lumia950::kGpuSmmuPhys; bytes = lumia950::kGpuSmmuSize; break;
    }
    if (!value || (offset & 3) || bytes < 4 || offset > bytes - 4) { return false; }
    asm volatile("dsb sy" ::: "memory");
    *value = *reinterpret_cast<volatile UINT32 *>(base + offset);
    asm volatile("dsb sy" ::: "memory");
    return true;
}

static void ReportContext(adreno::u32 base, const char *name)
{
    using namespace adreno;
    const u32 offsets[] = {0, 0x10, 0x20, 0x24, 0x30, 0x38, 0x3c, 0x58, 0x60, 0x64};
    const char *labels[] = {"SCTLR", "TCR2", "TTBR0_LO", "TTBR0_HI", "TCR",
                            "MAIR0", "MAIR1", "FSR", "FAR_LO", "FAR_HI"};
    Print(name); Print(" context (read only):\r\n");
    for (UINTN index = 0; index < sizeof(offsets) / sizeof(offsets[0]); index++) {
        u32 value = 0;
        if (!MmioRead(nullptr, RegisterSpace::Smmu, base + offsets[index], &value)) { return; }
        PrintHex(labels[index], value);
    }
}

extern "C" EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *firmware)
{
    system_table = firmware;
    report_bytes = 0;
    report_overflow = false;
    if (!firmware || !firmware->ConOut || !firmware->ConIn || !firmware->BootServices) {
        return EFI_INVALID_PARAMETER;
    }
    firmware->ConOut->ClearScreen(firmware->ConOut);
    Print("LUMIA 950 | GPU PREFLIGHT v2\r\n");
    Print("No GPU, power, clock, SMMU or firmware-variable writes.\r\n");
    Print("This is NOT GPU acceleration or iOS.\r\n");
    if (!AcpiMatches()) {
        Print("PLATFORM_UNSUPPORTED: MMIO reads skipped.\r\n");
    } else {
        Print("PLATFORM_MSM8992: six expected CPUs and GICv2 verified.\r\n");
        const ProbeFirmwareClocks clocks = ProbeFindFirmwareClocks(firmware->BootServices);
        PrintHex("FIRMWARE_CLOCK_STATUS", clocks.status);
        Print("FIRMWARE_CLOCK_PROTOCOL="); Print(clocks.protocolPresent ? "PRESENT\r\n" : "UNAVAILABLE\r\n");
        if (clocks.protocolPresent) {
            Print("FW_CORE_CLOCK="); Print(clocks.coreListed ? "LISTED\r\n" : "NOT_REGISTERED\r\n");
            Print("FW_AHB_CLOCK="); Print(clocks.ahbListed ? "LISTED\r\n" : "NOT_REGISTERED\r\n");
            Print("FW_TIMER_CLOCK="); Print(clocks.timerListed ? "LISTED\r\n" : "NOT_REGISTERED\r\n");
            Print("Clock lookup only; no enable or rate calls.\r\n");
        }
        using namespace adreno;
        const RegisterIo io = {nullptr, MmioRead, nullptr, nullptr, nullptr};
        GpuSnapshot snapshot = {};
        const ProbeResult result = readSnapshot(io, &snapshot);
        PrintHex("GDSCR_GX", snapshot.power.gx);
        PrintHex("GDSCR_CX", snapshot.power.cx);
        PrintHex("CORE_CBCR", snapshot.power.coreClock);
        PrintHex("AHB_CBCR", snapshot.power.ahbClock);
        PrintHex("TIMER_CBCR", snapshot.power.timerClock);
        Print("GPU_STATE="); Print(probeResultName(result)); Print("\r\n");
        if (result == ProbeResult::Ready) {
            PrintHex("RBBM_VERSION", snapshot.version);
            PrintHex("RBBM_CONFIGURATION", snapshot.configuration);
            PrintHex("RBBM_STATUS", snapshot.status);
            PrintHex("RBBM_INT_STATUS", snapshot.interruptStatus);
            PrintHex("CP_RB_BASE", snapshot.ringBase);
            PrintHex("CP_RB_CNTL", snapshot.ringControl);
            PrintHex("CP_RB_RPTR", snapshot.readPointer);
            PrintHex("CP_RB_WPTR", snapshot.writePointer);
            PrintHex("CP_ME_CNTL", snapshot.meControl);
            if ((snapshot.status & kGpuBusy) == 0) {
                ReportContext(0x8000, "USER");
                ReportContext(0x9000, "PRIV");
            } else { Print("SMMU context reads skipped: GPU is busy.\r\n"); }
        } else { Print("GPU and SMMU windows were NOT touched.\r\n"); }
    }
    Print("GPU PROBE COMPLETE\r\n");
    ProbeReportResult saved = {};
    if (report_overflow) { saved.status = EFI_BAD_BUFFER_SIZE; }
    else { ProbeSaveReport(firmware->BootServices, image, report_buffer, report_bytes, &saved); }
    Print("REPORT_STAGE="); Print(ProbeReportStageName(saved.stage)); Print("\r\n");
    PrintHex("REPORT_EFI_STATUS", saved.status);
    if (EFI_ERROR(saved.status)) { Print("Report not saved; photograph this screen.\r\n"); }
    else {
        char filename[48];
        UINTN index = 0;
        while (index + 1 < sizeof(filename) && saved.filename[index]) {
            filename[index] = static_cast<char>(saved.filename[index]);
            ++index;
        }
        filename[index] = 0;
        Print("Saved "); Print(filename); Print("\r\n");
    }
    Print("Press a key to return to firmware.\r\n");
    EFI_INPUT_KEY key = {};
    while (firmware->ConIn->ReadKeyStroke(firmware->ConIn, &key) == EFI_NOT_READY) {
        UINTN index = 0;
        const EFI_STATUS waited = firmware->BootServices->WaitForEvent(
            1, &firmware->ConIn->WaitForKey, &index);
        if (EFI_ERROR(waited)) { return waited; }
    }
    Print("Returning to firmware.\r\n");
    return EFI_SUCCESS;
}
