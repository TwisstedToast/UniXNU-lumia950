#include "../ReportStore.h"
#include "../FirmwareClock.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
unsigned checks;
unsigned failures;
void Expect(bool condition, const char *label)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

struct Environment {
    static Environment *active;
    EFI_BOOT_SERVICES services = {};
    EFI_LOADED_IMAGE_PROTOCOL loaded = {};
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL filesystem = {};
    EFI_FILE_PROTOCOL root = {};
    EFI_FILE_PROTOCOL file = {};
    struct LookupProtocol {VOID *(EFIAPI *get)(const CHAR8 *);} clock = {GetClock};
    std::array<bool, 64> slots = {};
    std::array<std::string, 64> reports;
    std::vector<std::string> clockNames;
    ProbeReportStage failAt = ProbeReportStage::Complete;
    EFI_STATUS failStatus = EFI_INVALID_PARAMETER;
    EFI_STATUS locateStatus = EFI_SUCCESS;
    UINTN currentSlot = 0;
    unsigned writes = 0;
    unsigned flushes = 0;
    unsigned fileCloses = 0;
    unsigned rootCloses = 0;
    unsigned handleCalls = 0;
    unsigned metadataCalls = 0;
    bool existing = false;
    bool open = false;
    bool partialWrite = false;
    bool secondaryCloseError = false;
    bool coreClock = true;
    bool ahbClock = false;
    bool timerClock = true;

    Environment()
    {
        active = this;
        loaded.DeviceHandle = Device();
        services.HandleProtocol = Handle;
        services.LocateProtocol = Locate;
        filesystem.OpenVolume = OpenVolume;
        root.Open = Open;
        root.Close = CloseRoot;
        file.Write = Write;
        file.Flush = Flush;
        file.Close = CloseFile;
        file.GetInfo = GetInfo;
        file.SetInfo = SetInfo;
        file.SetPosition = SetPosition;
    }

    static EFI_HANDLE Image() {return reinterpret_cast<EFI_HANDLE>(static_cast<UINTN>(0x10));}
    static EFI_HANDLE Device() {return reinterpret_cast<EFI_HANDLE>(static_cast<UINTN>(0x20));}

    static EFI_STATUS EFIAPI Handle(EFI_HANDLE handle, EFI_GUID *, VOID **protocol)
    {
        auto &env = *active;
        ++env.handleCalls;
        if (handle == Image()) {
            if (env.failAt == ProbeReportStage::LoadedImage) {return env.failStatus;}
            *protocol = &env.loaded;
        } else {
            Expect(handle == Device(), "only the loaded image's own filesystem is queried");
            if (env.failAt == ProbeReportStage::Filesystem) {return env.failStatus;}
            *protocol = &env.filesystem;
        }
        return EFI_SUCCESS;
    }

    static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **volume)
    {
        if (active->failAt == ProbeReportStage::OpenVolume) {return active->failStatus;}
        *volume = &active->root;
        return EFI_SUCCESS;
    }

    static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **output,
                                  CHAR16 *name, UINT64 mode, UINT64 attributes)
    {
        auto &env = *active;
        std::string path;
        for (UINTN index = 0; index < 48 && name[index]; ++index) path += static_cast<char>(name[index]);
        Expect(path.size() == 33 && path.substr(0, 27) == "\\EFI\\LumiaProbe\\gpu-report-", "report remains in the probe's own directory");
        Expect(name[27] >= '0' && name[27] <= '6' && name[28] >= '0' && name[28] <= '9', "bounded decimal report slot is formatted correctly");
        const UINTN slot = (name[27] - '0') * 10 + name[28] - '0';
        Expect(slot < 64 && attributes == 0, "report slot and create attributes are valid");
        if (mode == EFI_FILE_MODE_READ) {
            if (env.failAt == ProbeReportStage::FindUnusedName) {return env.failStatus;}
            if (!env.slots[slot]) {return EFI_NOT_FOUND;}
            env.existing = true;
        } else {
            Expect(mode == (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE), "new file uses the standard UEFI create mode");
            Expect(!env.slots[slot], "existing report is never opened for overwrite");
            if (env.failAt == ProbeReportStage::CreateFile) {return env.failStatus;}
            env.slots[slot] = true;
            env.existing = false;
        }
        env.currentSlot = slot;
        env.open = true;
        *output = &env.file;
        return EFI_SUCCESS;
    }

    static EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *, UINTN *size, VOID *bytes)
    {
        auto &env = *active;
        ++env.writes;
        Expect(env.open && !env.existing, "payload is written only to a newly opened file");
        if (env.failAt == ProbeReportStage::WriteFile) {return env.failStatus;}
        if (env.partialWrite) --*size;
        env.reports[env.currentSlot].assign(static_cast<const char *>(bytes), *size);
        return EFI_SUCCESS;
    }

    static EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *)
    {
        ++active->flushes;
        return active->failAt == ProbeReportStage::FlushFile ? active->failStatus : EFI_SUCCESS;
    }

    static EFI_STATUS EFIAPI CloseFile(EFI_FILE_PROTOCOL *)
    {
        auto &env = *active;
        ++env.fileCloses;
        env.open = false;
        if (env.secondaryCloseError) {return EFI_DEVICE_ERROR;}
        if (env.failAt == (env.existing ? ProbeReportStage::CloseExisting : ProbeReportStage::CloseFile)) {
            return env.failStatus;
        }
        return EFI_SUCCESS;
    }

    static EFI_STATUS EFIAPI CloseRoot(EFI_FILE_PROTOCOL *)
    {
        Expect(!active->open, "child file closes before its volume handle");
        ++active->rootCloses;
        return active->failAt == ProbeReportStage::CloseVolume ? active->failStatus : EFI_SUCCESS;
    }

    static EFI_STATUS EFIAPI GetInfo(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, VOID *)
    {
        ++active->metadataCalls;
        return EFI_INVALID_PARAMETER;
    }

    static EFI_STATUS EFIAPI SetInfo(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN, VOID *)
    {
        ++active->metadataCalls;
        return EFI_INVALID_PARAMETER;
    }

    static EFI_STATUS EFIAPI SetPosition(EFI_FILE_PROTOCOL *, UINT64)
    {
        ++active->metadataCalls;
        return EFI_INVALID_PARAMETER;
    }

    static EFI_STATUS EFIAPI Locate(EFI_GUID *guid, VOID *registration, VOID **protocol)
    {
        Expect(guid->Data1 == 0x4fcc91c2 && guid->Data2 == 0x9c4f && guid->Data3 == 0x4e3c &&
               guid->Data4[0] == 0xa6 && guid->Data4[7] == 0xd5, "firmware clock lookup uses the pinned Lumia protocol GUID");
        Expect(!registration, "clock discovery uses no notify registration");
        if (EFI_ERROR(active->locateStatus)) {return active->locateStatus;}
        *protocol = &active->clock;
        return EFI_SUCCESS;
    }

    static VOID *EFIAPI GetClock(const CHAR8 *name)
    {
        auto &env = *active;
        env.clockNames.emplace_back(name);
        const bool available = (!std::strcmp(name, "oxili_gfx3d_clk") && env.coreClock) ||
                               (!std::strcmp(name, "oxilicx_ahb_clk") && env.ahbClock) ||
                               (!std::strcmp(name, "oxili_rbbmtimer_clk") && env.timerClock);
        return available ? reinterpret_cast<VOID *>(static_cast<UINTN>(0x100)) : nullptr;
    }
};
Environment *Environment::active;
const char payload[] = "GPU PREFLIGHT\r\nGPU PROBE COMPLETE\r\n";

EFI_STATUS Save(Environment &env, ProbeReportResult &result)
{
    return ProbeSaveReport(&env.services, Environment::Image(), payload, sizeof(payload) - 1, &result);
}

void TestFreshAndRepeat()
{
    Environment env;
    ProbeReportResult result;
    Expect(Save(env, result) == EFI_SUCCESS, "fresh report saves even when metadata APIs reject every request");
    Expect(result.stage == ProbeReportStage::Complete && result.created && result.slot == 0, "successful report exposes slot and completion");
    Expect(env.reports[0] == payload, "saved report exactly matches the requested bytes");
    Expect(!env.metadataCalls && env.writes == 1 && env.flushes == 1, "fresh report needs no GetInfo, SetInfo or seek");
    const std::string previous = env.reports[0];
    Expect(Save(env, result) == EFI_SUCCESS && result.slot == 1, "repeat boot selects the next unused report slot");
    Expect(env.reports[0] == previous && env.reports[1] == payload, "previous report is preserved byte-for-byte");
    Expect(env.fileCloses == 3 && env.rootCloses == 2, "each existing/new file and volume handle is closed");
}

void TestFailures()
{
    for (ProbeReportStage stage : {ProbeReportStage::LoadedImage, ProbeReportStage::Filesystem,
         ProbeReportStage::OpenVolume, ProbeReportStage::FindUnusedName, ProbeReportStage::CreateFile,
         ProbeReportStage::WriteFile, ProbeReportStage::FlushFile, ProbeReportStage::CloseFile,
         ProbeReportStage::CloseVolume}) {
        Environment env;
        env.failAt = stage;
        ProbeReportResult result;
        Expect(Save(env, result) == EFI_INVALID_PARAMETER, "original EFI status survives cleanup");
        Expect(result.stage == stage && result.status == EFI_INVALID_PARAMETER, "exact failing operation is reported");
        Expect(std::strcmp(ProbeReportStageName(result.stage), "UNKNOWN") != 0, "failing operation has a printable stage name");
        Expect(!env.open && !env.metadataCalls, "failed save leaves no open file or metadata rewrite");
        if (stage < ProbeReportStage::WriteFile) Expect(env.writes == 0, "early failure writes no payload");
    }
    Environment env;
    env.partialWrite = true;
    ProbeReportResult result;
    Expect(Save(env, result) == EFI_DEVICE_ERROR && result.stage == ProbeReportStage::WriteFile, "short successful Write is not accepted as a saved report");
    Expect(env.flushes == 0 && env.fileCloses == 1 && env.rootCloses == 1, "partial write closes both handles without claiming flush success");
    Environment writeProtected;
    writeProtected.failAt = ProbeReportStage::CreateFile;
    writeProtected.failStatus = EFI_WRITE_PROTECTED;
    Expect(Save(writeProtected, result) == EFI_WRITE_PROTECTED, "read-only volume is diagnosed separately");
    Expect(!writeProtected.writes && !result.created, "read-only volume creates no report");
    Environment dualError;
    dualError.failAt = ProbeReportStage::WriteFile;
    dualError.secondaryCloseError = true;
    Expect(Save(dualError, result) == EFI_INVALID_PARAMETER, "close failure does not hide the first write error");
    Expect(result.fileCloseStatus == EFI_DEVICE_ERROR, "secondary close status is retained");
    Environment incomplete;
    incomplete.file.Write = nullptr;
    Expect(Save(incomplete, result) == EFI_PROTOCOL_ERROR, "incomplete file protocol is refused");
    Expect(incomplete.fileCloses == 1 && incomplete.rootCloses == 1, "incomplete writable file still closes both handles");
}

void TestBounds()
{
    Environment full;
    full.slots.fill(true);
    ProbeReportResult result;
    Expect(Save(full, result) == EFI_VOLUME_FULL && !full.writes, "64 occupied slots are refused instead of overwritten");
    Expect(full.fileCloses == 64 && full.rootCloses == 1, "slot search is bounded and closes every probe handle");
    Environment existing;
    existing.slots[0] = true;
    existing.failAt = ProbeReportStage::CloseExisting;
    Expect(Save(existing, result) == EFI_INVALID_PARAMETER && result.stage == ProbeReportStage::CloseExisting,
           "failed close during existing-name probe stops before creation");
    Environment missingDevice;
    missingDevice.loaded.DeviceHandle = nullptr;
    Expect(Save(missingDevice, result) == EFI_NOT_FOUND && result.stage == ProbeReportStage::LoadedDevice,
           "memory-loaded image does not write a guessed filesystem");
    Expect(missingDevice.handleCalls == 1 && missingDevice.rootCloses == 0, "missing image device never reaches filesystem calls");
    Environment invalid;
    Expect(ProbeSaveReport(&invalid.services, Environment::Image(), payload, 0, &result) == EFI_INVALID_PARAMETER,
           "empty report is refused");
    Expect(ProbeSaveReport(&invalid.services, Environment::Image(), payload, 8193, &result) == EFI_INVALID_PARAMETER,
           "oversized report is refused before reading its data");
    Expect(ProbeSaveReport(nullptr, Environment::Image(), payload, 1, &result) == EFI_INVALID_PARAMETER,
           "missing services are refused");
    Expect(!invalid.handleCalls, "invalid report arguments call no firmware services");
}

void TestClocks()
{
    Environment env;
    const ProbeFirmwareClocks clocks = ProbeFindFirmwareClocks(&env.services);
    Expect(clocks.status == EFI_SUCCESS && clocks.protocolPresent, "clock registry protocol can be discovered");
    Expect(clocks.coreListed && !clocks.ahbListed && clocks.timerListed, "clock registry results are distinct from hardware clock state");
    Expect(env.clockNames.size() == 3, "only three named clock lookups are performed");
    Expect(env.clockNames[0] == "oxili_gfx3d_clk" && env.clockNames[1] == "oxilicx_ahb_clk" &&
           env.clockNames[2] == "oxili_rbbmtimer_clk", "clock names match the MSM8992 references");
    Environment absent;
    absent.locateStatus = EFI_NOT_FOUND;
    const ProbeFirmwareClocks missing = ProbeFindFirmwareClocks(&absent.services);
    Expect(missing.status == EFI_NOT_FOUND && !missing.protocolPresent && absent.clockNames.empty(),
           "absent protocol calls no clock methods");
    Environment unavailable;
    unavailable.clock.get = nullptr;
    Expect(ProbeFindFirmwareClocks(&unavailable.services).status == EFI_PROTOCOL_ERROR,
           "missing lookup method is not dereferenced");
    Environment handset;
    handset.coreClock = handset.ahbClock = handset.timerClock = false;
    const ProbeFirmwareClocks handsetClocks = ProbeFindFirmwareClocks(&handset.services);
    Expect(handsetClocks.status == EFI_SUCCESS && handsetClocks.protocolPresent,
           "v2 handset protocol lookup succeeds even when all three queried clocks are absent");
    Expect(!handsetClocks.coreListed && !handsetClocks.ahbListed && !handsetClocks.timerListed,
           "protocol presence never substitutes for missing GPU clock registrations");
    Expect(handset.clockNames == std::vector<std::string>{"oxili_gfx3d_clk", "oxilicx_ahb_clk", "oxili_rbbmtimer_clk"},
           "missing clock entries do not trigger guessed aliases or a different operation");
}
}

int main()
{
    TestFreshAndRepeat();
    TestFailures();
    TestBounds();
    TestClocks();
    std::printf("UEFI report/clock lookup: %u checks, %u failures (mock firmware, no GPU access)\n", checks, failures);
    return failures ? 1 : 0;
}
