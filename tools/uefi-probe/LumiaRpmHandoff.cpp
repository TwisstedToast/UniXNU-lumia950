#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>

#include "FirmwareSmem.h"
#include "FirmwareMemory.h"
#include "PlatformCheck.h"
#include "ReportStore.h"
#include "SmdMemory.h"

extern "C" {
#include "FramebufferCore.h"
#include "ProbeCore.h"
}

using namespace adreno;
using namespace adreno::rpm;

namespace {
EFI_SYSTEM_TABLE *firmware;
char report[8192];
UINTN reportBytes;
bool reportOverflow;
UINT64 timerFrequency;

void Print(const char *text)
{
    CHAR16 console[96];
    UINTN used = 0;
    while (*text) {
        const char value = *text++;
        if (reportBytes < sizeof(report)) { report[reportBytes++] = value; }
        else { reportOverflow = true; }
        console[used++] = static_cast<unsigned char>(value);
        if (used == 95 || !*text) {
            console[used] = 0;
            firmware->ConOut->OutputString(firmware->ConOut, console);
            used = 0;
        }
    }
}

void PrintHex(const char *label, UINT64 value)
{
    char digits[17];
    Print(label); Print("=0x");
    if (ProbeFormatUnsigned(value, 16, 16, digits, sizeof(digits))) { Print(digits); }
    Print("\r\n");
}

UINT32 Read32(const UINT8 *value)
{
    return value[0] | (static_cast<UINT32>(value[1]) << 8) |
        (static_cast<UINT32>(value[2]) << 16) | (static_cast<UINT32>(value[3]) << 24);
}

UINT64 Read64(const UINT8 *value)
{
    return Read32(value) | (static_cast<UINT64>(Read32(value + 4)) << 32);
}

bool Checksum(const UINT8 *bytes, UINTN count)
{
    UINT8 sum = 0;
    for (UINTN index = 0; index < count; ++index) { sum = static_cast<UINT8>(sum + bytes[index]); }
    return sum == 0;
}

bool LumiaPlatform()
{
    const EFI_GUID expected = EFI_ACPI_20_TABLE_GUID;
    for (UINTN index = 0; index < firmware->NumberOfTableEntries; ++index) {
        const EFI_CONFIGURATION_TABLE &entry = firmware->ConfigurationTable[index];
        const UINT8 *actual = reinterpret_cast<const UINT8 *>(&entry.VendorGuid);
        const UINT8 *wanted = reinterpret_cast<const UINT8 *>(&expected);
        bool matches = true;
        for (UINTN position = 0; position < sizeof(EFI_GUID); ++position) {
            if (actual[position] != wanted[position]) { matches = false; }
        }
        if (!matches || !entry.VendorTable) { continue; }
        const UINT8 *rsdp = static_cast<const UINT8 *>(entry.VendorTable);
        if (Read64(rsdp) != 0x2052545020445352ull || rsdp[15] < 2 || !Checksum(rsdp, 20)) { return false; }
        const UINT32 length = Read32(rsdp + 20);
        if (length < 36 || length > 4096 || !Checksum(rsdp, length)) { return false; }
        const UINT64 address = Read64(rsdp + 24);
        if (!address || (address & 3)) { return false; }
        const UINT8 *xsdt = reinterpret_cast<const UINT8 *>(address);
        const UINT32 size = Read32(xsdt + 4);
        if (Read32(xsdt) != 0x54445358 || size < 36 || size > 65536 ||
            (size - 36) % 8 || !Checksum(xsdt, size)) { return false; }
        for (UINT32 offset = 36; offset < size; offset += 8) {
            const UINT64 tableAddress = Read64(xsdt + offset);
            if (!tableAddress || (tableAddress & 3)) { continue; }
            const UINT8 *table = reinterpret_cast<const UINT8 *>(tableAddress);
            if (Read32(table) == 0x43495041) { return isLumia950Madt(table, Read32(table + 4)); }
        }
        return false;
    }
    return false;
}

bool Translate(void *, u64 address, bool writing, u64 *par)
{
    UINT64 savedPar, savedDaif, translated;
    if (!par) { return false; }
    asm volatile("mrs %0, daif\nmsr daifset, #15\nmrs %1, par_el1"
                 : "=r"(savedDaif), "=r"(savedPar) :: "memory");
    if (writing) {
        asm volatile("at s1e1w, %1\nisb\nmrs %0, par_el1" : "=r"(translated) : "r"(address) : "memory");
    } else {
        asm volatile("at s1e1r, %1\nisb\nmrs %0, par_el1" : "=r"(translated) : "r"(address) : "memory");
    }
    asm volatile("msr par_el1, %0\nmsr daif, %1" :: "r"(savedPar), "r"(savedDaif) : "memory");
    *par = translated;
    return true;
}

bool NativeRead(void *, u64 address, u32 *value)
{
    if (!value || (address & 3)) { return false; }
    asm volatile("dsb sy" ::: "memory");
    *value = *reinterpret_cast<volatile UINT32 *>(address);
    asm volatile("dsb sy" ::: "memory");
    return true;
}

bool NativeWrite(void *, u64 address, u32 value)
{
    if (address & 3) { return false; }
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile UINT32 *>(address) = value;
    asm volatile("dsb sy" ::: "memory");
    return true;
}

bool Barrier(void *)
{
    asm volatile("dsb sy" ::: "memory");
    return true;
}

UINT64 Counter()
{
    UINT64 value;
    asm volatile("isb\nmrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}

void Delay(void *, u32 microseconds)
{
    const UINT64 start = Counter();
    const UINT64 ticks = (timerFrequency * microseconds + 999999) / 1000000;
    while (Counter() - start < ticks) { asm volatile("yield"); }
}

const char *ResultName(Result result)
{
    switch (result) {
    case Result::Ok: return "OK";
    case Result::Invalid: return "INVALID";
    case Result::Unowned: return "UNOWNED";
    case Result::NotReady: return "NOT READY";
    case Result::IoError: return "IO ERROR";
    case Result::TimedOut: return "TIMED OUT";
    case Result::ProtocolError: return "PROTOCOL ERROR";
    case Result::RemoteError: return "REMOTE ERROR";
    case Result::MessageIdExhausted: return "ID EXHAUSTED";
    }
    return "UNKNOWN";
}

void SavePreflight(EFI_HANDLE image)
{
    ProbeReportResult saved = {};
    if (reportOverflow) { saved.status = EFI_BAD_BUFFER_SIZE; }
    else { ProbeSaveReport(firmware->BootServices, image, report, reportBytes, &saved); }
    Print("PREFLIGHT_SAVE_STAGE="); Print(ProbeReportStageName(saved.stage)); Print("\r\n");
    PrintHex("PREFLIGHT_SAVE_STATUS", saved.status);
    if (!EFI_ERROR(saved.status)) {
        char filename[48];
        UINTN index = 0;
        while (index + 1 < sizeof(filename) && saved.filename[index]) {
            filename[index] = static_cast<char>(saved.filename[index]);
            ++index;
        }
        filename[index] = 0;
        Print("Saved "); Print(filename); Print("\r\n");
    }
}

EFI_STATUS WaitForKey(EFI_INPUT_KEY *key)
{
    for (;;) {
        const EFI_STATUS read = firmware->ConIn->ReadKeyStroke(firmware->ConIn, key);
        if (read != EFI_NOT_READY) { return read; }
        UINTN index = 0;
        const EFI_STATUS waited = firmware->BootServices->WaitForEvent(1, &firmware->ConIn->WaitForKey, &index);
        if (EFI_ERROR(waited)) { return waited; }
    }
}

void Flush(const ProbeFramebuffer &framebuffer, UINT32 top, UINT32 rows)
{
    UINT64 cache;
    asm volatile("mrs %0, ctr_el0" : "=r"(cache));
    const UINT64 line = 4ull << ((cache >> 16) & 15);
    const UINT64 start = framebuffer.base + static_cast<UINT64>(top) * framebuffer.stride * 4;
    const UINT64 end = start + static_cast<UINT64>(rows) * framebuffer.stride * 4;
    for (UINT64 address = start & ~(line - 1); address < end; address += line) {
        asm volatile("dc cvac, %0" :: "r"(address) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

void Line(const ProbeFramebuffer &framebuffer, UINT32 row, const char *label, UINT32 color)
{
    const UINT32 scale = framebuffer.width >= 800 ? 3 : 1;
    ProbeDrawText(&framebuffer, 24, 40 + row * 12 * scale, scale, label, color);
}

void Number(const ProbeFramebuffer &framebuffer, UINT32 row, const char *label, UINT64 value, UINT32 color)
{
    const UINT32 scale = framebuffer.width >= 800 ? 3 : 1;
    char digits[17];
    Line(framebuffer, row, label, color);
    if (ProbeFormatUnsigned(value, 16, 8, digits, sizeof(digits))) {
        ProbeDrawText(&framebuffer, 24 + scale * 6 * 15, 40 + row * 12 * scale, scale, digits, color);
    }
}

[[noreturn]] void DisplayResult(const ProbeFramebuffer &framebuffer, const char *heading, bool passed,
                              Result claim, Result opened, Result closed, Result retired,
                              const ChannelSnapshot &after, const ChannelSnapshot &last, bool exited)
{
    asm volatile("msr daifset, #15" ::: "memory");
    const UINT32 color = ProbeFramebufferColor(framebuffer.pixel_format, passed ? 0 : 255, passed ? 255 : 100, 0);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, framebuffer.height, 0);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, 12, color);
    Line(framebuffer, 0, heading, color);
    Line(framebuffer, 1, exited ? "POST EBS - NATIVE SMD" : "NO EBS CONFIRMATION", color);
    Line(framebuffer, 2, "NO RESOURCE VOTES OR GPU WRITES", color);
    Line(framebuffer, 3, "CLAIM", color); Line(framebuffer, 4, ResultName(claim), color);
    Line(framebuffer, 5, "OPEN", color); Line(framebuffer, 6, ResultName(opened), color);
    Line(framebuffer, 7, "CLOSE", color); Line(framebuffer, 8, ResultName(closed), color);
    Line(framebuffer, 9, "RETIRE", color); Line(framebuffer, 10, ResultName(retired), color);
    Number(framebuffer, 12, "EBS LOCAL", after.words[0], color);
    Number(framebuffer, 13, "EBS REMOTE", after.words[11], color);
    Number(framebuffer, 14, "FINAL LOCAL", last.words[0], color);
    Number(framebuffer, 15, "FINAL REMOTE", last.words[11], color);
    Number(framebuffer, 16, "TX READ", last.words[9], color);
    Number(framebuffer, 17, "TX WRITE", last.words[10], color);
    Number(framebuffer, 18, "RX READ", last.words[20], color);
    Number(framebuffer, 19, "RX WRITE", last.words[21], color);
    Line(framebuffer, 21, "NOT GPU ACCELERATION OR IOS", color);
    Line(framebuffer, 22, "FORCE REBOOT TO EXIT", color);
    Flush(framebuffer, 0, framebuffer.height);
    const UINT32 top = framebuffer.height - 64;
    const UINT32 width = framebuffer.width / 8;
    const UINT32 slots = (framebuffer.width - width) / 24 + 1;
    UINT64 previous = ~static_cast<UINT64>(0);
    for (;;) {
        const UINT64 tick = Counter() / (timerFrequency / 8);
        if (tick != previous) {
            ProbeFillRectangle(&framebuffer, 0, top, framebuffer.width, 24, 0);
            ProbeFillRectangle(&framebuffer, static_cast<UINT32>(tick % slots) * 24, top, width, 24, color);
            Flush(framebuffer, top, 24);
            previous = tick;
        }
        asm volatile("yield");
    }
}

[[noreturn]] void AfterExit(const ProbeFramebuffer &framebuffer, MemoryBinding &binding)
{
    asm volatile("msr daifset, #15" ::: "memory");
    const UINT32 blue = ProbeFramebufferColor(framebuffer.pixel_format, 80, 160, 255);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, framebuffer.height, 0);
    Line(framebuffer, 0, "POST EBS - CHECKING RPM", blue);
    Line(framebuffer, 1, "NO GPU WRITES OR RESOURCE VOTES", blue);
    Flush(framebuffer, 0, framebuffer.height);
    Result claim = Result::NotReady;
    for (UINT32 attempt = 0; attempt < 200; ++attempt) {
        claim = claimMemory(binding, true, true);
        if (claim != Result::NotReady || binding.state != MemoryState::Observed) { break; }
        if (attempt + 1 < 200) { Delay(nullptr, 1000); }
    }
    ChannelSnapshot after = {}, last = {};
    Result opened = Result::NotReady, closed = Result::NotReady, retired = Result::NotReady;
    const Result sampled = snapshotChannel(binding, after);
    if (claim == Result::Ok && sampled == Result::Ok) {
        SmdPipe pipe = {};
        opened = openPipe(pipe, memoryIo(binding), 88, binding.layout.fifo.bytes / 2, true, true, 2000, 100);
        if (opened == Result::Ok) { closed = closePipe(pipe, 2000, 100); }
        snapshotChannel(binding, last);
        if (closed == Result::Ok) { retired = retireMemory(binding, pipe); }
    } else { last = after; }
    const bool passed = claim == Result::Ok && opened == Result::Ok && closed == Result::Ok && retired == Result::Ok;
    DisplayResult(framebuffer, passed ? "NATIVE RPM CHANNEL OK" : "RPM HANDOFF STOPPED", passed,
                  claim, opened, closed, retired, after, last, true);
}

}

extern "C" void *memset(void *destination, int value, size_t bytes)
{
    auto *output = static_cast<unsigned char *>(destination);
    for (size_t index = 0; index < bytes; ++index) { output[index] = static_cast<unsigned char>(value); }
    return destination;
}

extern "C" void *memcpy(void *destination, const void *source, size_t bytes)
{
    auto *output = static_cast<unsigned char *>(destination);
    const auto *input = static_cast<const unsigned char *>(source);
    for (size_t index = 0; index < bytes; ++index) { output[index] = input[index]; }
    return destination;
}

extern "C" EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable)
{
    if (!systemTable || !systemTable->BootServices || !systemTable->ConOut || !systemTable->ConIn ||
        !systemTable->ConIn->WaitForKey) { return EFI_INVALID_PARAMETER; }
    firmware = systemTable;
    reportBytes = 0;
    reportOverflow = false;
    firmware->ConOut->ClearScreen(firmware->ConOut);
    Print("LUMIA 950 | NATIVE RPM HANDOFF v1\r\n");
    Print("Optional post-EBS channel open/close; NO RPM packets or GPU writes.\r\n");
    if (!LumiaPlatform()) {
        Print("PLATFORM_UNSUPPORTED: no SMEM, translation or MMIO access.\r\n");
        Print("RPM HANDOFF REFUSED - firmware retained.\r\n");
        EFI_INPUT_KEY key = {};
        const EFI_STATUS status = WaitForKey(&key);
        Print("Returning to firmware.\r\n");
        return status;
    }
    UINT64 currentEl, control, affinity, identity;
    asm volatile("mrs %0, currentel\nmrs %1, sctlr_el1\nmrs %2, cntfrq_el0\nmrs %3, mpidr_el1\nmrs %4, midr_el1"
                 : "=r"(currentEl), "=r"(control), "=r"(timerFrequency), "=r"(affinity), "=r"(identity));
    if (((currentEl >> 2) & 3) != 1 || !(control & 1) || timerFrequency < 1000000 || timerFrequency > 1000000000) {
        Print("EL1, active MMU and qualified timer required.\r\n");
        return EFI_UNSUPPORTED;
    }
    PrintHex("BOOT_MPIDR", affinity); PrintHex("BOOT_MIDR", identity);
    if ((affinity & 0xff00ffffffull) || ((identity >> 24) & 0xff) != 0x41 ||
        ((identity >> 4) & 0xfff) != 0xd03) {
        Print("Expected affinity-zero A53 boot CPU; firmware retained.\r\n");
        SavePreflight(image);
        return EFI_UNSUPPORTED;
    }
    const ProbeFirmwareSmem smem = ProbeFindFirmwareSmem(firmware->BootServices);
    PrintHex("SMEM_STATUS", smem.status);
    PrintHex("RPM_CHANNEL", smem.channelId);
    PrintHex("RPM_TYPE", smem.channelType);
    PrintHex("RPM_CONTROL", smem.controlAddress);
    PrintHex("RPM_CONTROL_BYTES", smem.controlBytes);
    PrintHex("RPM_FIFO", smem.fifoAddress);
    PrintHex("RPM_FIFO_BYTES", smem.fifoBytes);
    PrintHex("RPM_IPC", kRpmDoorbellPhysical);
    if (EFI_ERROR(smem.status) || smem.channelCount != 1 || smem.channelId != 4 || smem.channelType != 0x020f ||
        smem.controlAddress != kMessageRamPhysical || smem.controlBytes != 88 ||
        smem.fifoAddress != kMessageRamPhysical + 0x200 || smem.fifoBytes != 2048) {
        Print("HANDSET_PROFILE_MISMATCH: no channel writes.\r\n");
        SavePreflight(image);
        return EFI_UNSUPPORTED;
    }
    const MemoryLayout layout = {{smem.controlAddress, smem.controlAddress, smem.controlBytes},
                                 {smem.fifoAddress, smem.fifoAddress, smem.fifoBytes},
                                 {kRpmDoorbellPhysical, kRpmDoorbellPhysical, 4}};
    const MemoryRegion regions[] = {layout.control, layout.fifo, layout.doorbell};
    const char *readLabels[] = {"CONTROL_PAR_R", "FIFO_PAR_R", "IPC_PAR_R"};
    const char *writeLabels[] = {"CONTROL_PAR_W", "FIFO_PAR_W", "IPC_PAR_W"};
    for (UINT32 index = 0; index < 3; ++index) {
        u64 par = 0;
        Translate(nullptr, regions[index].address, false, &par); PrintHex(readLabels[index], par);
        Translate(nullptr, regions[index].address, true, &par); PrintHex(writeLabels[index], par);
    }
    MemoryBinding binding = {};
    const MemoryAccess access = {nullptr, Translate, NativeRead, NativeWrite, Barrier, Delay};
    const Result observed = observeMemory(binding, layout, access);
    Print("DEVICE_TRANSLATIONS="); Print(ResultName(observed)); Print("\r\n");
    if (observed != Result::Ok) { SavePreflight(image); return EFI_UNSUPPORTED; }
    EFI_GUID graphicsGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *graphics = nullptr;
    const EFI_STATUS found = firmware->BootServices->LocateProtocol(&graphicsGuid, nullptr, reinterpret_cast<VOID **>(&graphics));
    if (EFI_ERROR(found) || !graphics || !graphics->Mode || !graphics->Mode->Info) { return EFI_UNSUPPORTED; }
    const auto *mode = graphics->Mode;
    const ProbeFramebuffer framebuffer = {mode->FrameBufferBase, mode->FrameBufferSize,
        mode->Info->HorizontalResolution, mode->Info->VerticalResolution,
        mode->Info->PixelsPerScanLine, static_cast<UINT32>(mode->Info->PixelFormat)};
    if (!ProbeFramebufferValid(&framebuffer) || framebuffer.width < 640 || framebuffer.height < 1000) { return EFI_UNSUPPORTED; }
    EFI_GUID loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = nullptr;
    EFI_STATUS status = firmware->BootServices->HandleProtocol(image, &loadedGuid, reinterpret_cast<VOID **>(&loaded));
    if (EFI_ERROR(status) || !loaded || ProbeRangesOverlap(framebuffer.base, framebuffer.byte_size,
        reinterpret_cast<UINTN>(loaded->ImageBase), loaded->ImageSize)) { return EFI_COMPROMISED_DATA; }
    for (const MemoryRegion &region : regions) {
        if (ProbeRangesOverlap(framebuffer.base, framebuffer.byte_size, region.physical, region.bytes)) {
            return EFI_COMPROMISED_DATA;
        }
    }
    UINTN mapBytes = 0, mapKey = 0, stride = 0;
    UINT32 version = 0;
    EFI_BOOT_SERVICES *services = firmware->BootServices;
    status = services->GetMemoryMap(&mapBytes, nullptr, &mapKey, &stride, &version);
    if (status != EFI_BUFFER_TOO_SMALL || stride < sizeof(EFI_MEMORY_DESCRIPTOR) || stride > 4096 ||
        mapBytes > 16 * 1024 * 1024) { return EFI_COMPROMISED_DATA; }
    const UINTN capacity = mapBytes + stride * 64;
    UINT8 *map = nullptr;
    status = services->AllocatePool(EfiLoaderData, capacity, reinterpret_cast<VOID **>(&map));
    if (EFI_ERROR(status)) { return status; }
    mapBytes = capacity;
    status = services->GetMemoryMap(&mapBytes, reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(map), &mapKey, &stride, &version);
    const bool qualified = !EFI_ERROR(status) && mapBytes <= capacity && ProbeRpmMmioReserved(map, mapBytes, stride, layout);
    Print("EFI_MMIO_RESERVATIONS="); Print(qualified ? "QUALIFIED\r\n" : "REFUSED\r\n");
    if (qualified) {
        ChannelSnapshot snapshot = {};
        if (snapshotChannel(binding, snapshot) != Result::Ok) {
            services->FreePool(map);
            SavePreflight(image);
            return EFI_DEVICE_ERROR;
        }
        PrintHex("FW_LOCAL_STATE", snapshot.words[0]);
        PrintHex("FW_REMOTE_STATE", snapshot.words[11]);
        PrintHex("FW_LOCAL_DTR", snapshot.words[1]); PrintHex("FW_REMOTE_DTR", snapshot.words[12]);
        PrintHex("FW_TX_READ", snapshot.words[9]); PrintHex("FW_TX_WRITE", snapshot.words[10]);
        PrintHex("FW_RX_READ", snapshot.words[20]); PrintHex("FW_RX_WRITE", snapshot.words[21]);
    }
    Print("No channel, FIFO or IPC writes before ExitBootServices.\r\n");
    SavePreflight(image);
    if (!qualified) { services->FreePool(map); return EFI_UNSUPPORTED; }
    Print("RPM HANDOFF READY - Volume Up starts the native test.\r\n");
    Print("Any other key returns. Post-EBS results are screen-only.\r\n");
    EFI_INPUT_KEY key = {};
    status = WaitForKey(&key);
    if (EFI_ERROR(status) || key.ScanCode != SCAN_UP) {
        Print("RPM HANDOFF CANCELLED - firmware retained.\r\n");
        services->FreePool(map);
        return EFI_ERROR(status) ? status : EFI_SUCCESS;
    }
    status = services->SetWatchdogTimer(0, 0, 0, nullptr);
    if (EFI_ERROR(status)) { services->FreePool(map); return status; }
    const auto getMap = services->GetMemoryMap;
    const auto exitServices = services->ExitBootServices;
    Print("Attempting native RPM handoff; force reboot after result.\r\n");
    const UINT32 orange = ProbeFramebufferColor(framebuffer.pixel_format, 255, 140, 0);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, framebuffer.height, 0);
    Line(framebuffer, 0, "BEFORE EBS - RPM HANDOFF", orange);
    Flush(framebuffer, 0, framebuffer.height);
    for (UINT32 attempt = 0; attempt < 3; ++attempt) {
        mapBytes = capacity;
        status = getMap(&mapBytes, reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(map), &mapKey, &stride, &version);
        if (EFI_ERROR(status) || mapBytes > capacity || !ProbeRpmMmioReserved(map, mapBytes, stride, layout)) { break; }
        status = exitServices(image, mapKey);
        if (!EFI_ERROR(status)) { AfterExit(framebuffer, binding); }
        if (status != EFI_INVALID_PARAMETER) { break; }
    }
    const ChannelSnapshot empty = {};
    DisplayResult(framebuffer, "EXIT BOOT SERVICES FAILED", false, Result::NotReady,
                  Result::NotReady, Result::NotReady, Result::NotReady, empty, empty, false);
}
