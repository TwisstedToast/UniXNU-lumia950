#include "../FirmwareGcd.h"
#include <Guid/DxeServices.h>
#include <array>
#include <cstdio>

using namespace adreno::rpm;

namespace {
unsigned checks;
unsigned failures;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", message); }
}

const MemoryLayout layout = {{kMessageRamPhysical, kMessageRamPhysical, 88},
                             {kMessageRamPhysical + 0x200, kMessageRamPhysical + 0x200, 2048},
                             {kRpmDoorbellPhysical, kRpmDoorbellPhysical, 4}};

struct Environment {
    static Environment *active;
    EFI_SYSTEM_TABLE firmware = {};
    std::array<EFI_CONFIGURATION_TABLE, 2> tables = {};
    EFI_DXE_SERVICES services = {};
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR message = {}, ipc = {};
    std::array<UINT64, 6> addresses = {};
    unsigned calls = 0, failAt = 0;
    bool changeSecond = false;

    Environment()
    {
        active = this;
        firmware.ConfigurationTable = tables.data(); firmware.NumberOfTableEntries = 1;
        tables[0].VendorGuid = DXE_SERVICES_TABLE_GUID; tables[0].VendorTable = &services;
        services.Hdr.Signature = DXE_SERVICES_SIGNATURE; services.Hdr.HeaderSize = sizeof(services);
        services.GetMemorySpaceDescriptor = Get;
        message.BaseAddress = kMessageRamPhysical; message.Length = kMessageRamBytes;
        message.Capabilities = EFI_MEMORY_UC; message.Attributes = EFI_MEMORY_UC;
        message.GcdMemoryType = EfiGcdMemoryTypeMemoryMappedIo;
        ipc = message; ipc.BaseAddress = 0xf9000000; ipc.Length = 0x113000;
    }

    static EFI_STATUS EFIAPI Get(EFI_PHYSICAL_ADDRESS address, EFI_GCD_MEMORY_SPACE_DESCRIPTOR *descriptor)
    {
        auto &environment = *active;
        if (environment.calls < environment.addresses.size()) { environment.addresses[environment.calls] = address; }
        ++environment.calls;
        if (environment.calls == environment.failAt) { return EFI_DEVICE_ERROR; }
        *descriptor = address >= kMessageRamPhysical ? environment.message : environment.ipc;
        if (environment.changeSecond && environment.calls == 2) { descriptor->Attributes ^= EFI_MEMORY_WB; }
        return EFI_SUCCESS;
    }
    ProbeRpmGcd find() { return ProbeFindRpmGcd(&firmware, layout); }
};
Environment *Environment::active;
}

int main()
{
    Environment valid;
    const ProbeRpmGcd result = valid.find();
    expect(result.status == EFI_SUCCESS && result.qualified && result.tablePresent && result.checkedRegions == 3,
           "DXE GCD independently classifies both shared-memory allocations and IPC as MMIO");
    expect(valid.calls == 6 && valid.addresses == std::array<UINT64, 6>{kMessageRamPhysical,
        kMessageRamPhysical + 87, kMessageRamPhysical + 0x200, kMessageRamPhysical + 0x9ff,
        kRpmDoorbellPhysical, kRpmDoorbellPhysical + 3}, "only the beginning and final byte of each bounded allocation are queried");
    expect(ProbeFindRpmGcd(nullptr, layout).status == EFI_INVALID_PARAMETER, "missing system table is refused");
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        Environment invalid;
        switch (mutation) {
        case 0: invalid.firmware.ConfigurationTable = nullptr; break;
        case 1: invalid.firmware.NumberOfTableEntries = 0; break;
        case 2: invalid.firmware.NumberOfTableEntries = 4097; break;
        case 3: invalid.tables[0].VendorGuid.Data1 ^= 1; break;
        case 4: invalid.tables[0].VendorTable = nullptr; break;
        case 5: invalid.services.Hdr.Signature ^= 1; break;
        case 6: invalid.services.Hdr.HeaderSize = 24; break;
        case 7: invalid.services.Hdr.HeaderSize = 4097; break;
        case 8: invalid.services.GetMemorySpaceDescriptor = nullptr; break;
        case 9: invalid.tables[1] = invalid.tables[0]; invalid.firmware.NumberOfTableEntries = 2; break;
        case 10: invalid.tables[0].VendorTable = reinterpret_cast<VOID *>(reinterpret_cast<UINTN>(&invalid.services) + 1); break;
        }
        const ProbeRpmGcd rejected = invalid.find();
        expect(EFI_ERROR(rejected.status) && !rejected.qualified && invalid.calls == 0,
               "missing, ambiguous or malformed DXE table is refused before calling a service");
    }
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        Environment invalid;
        switch (mutation) {
        case 0: invalid.message.GcdMemoryType = EfiGcdMemoryTypeSystemMemory; break;
        case 1: invalid.message.GcdMemoryType = EfiGcdMemoryTypeReserved; break;
        case 2: invalid.message.GcdMemoryType = EfiGcdMemoryTypeNonExistent; break;
        case 3: invalid.message.Capabilities = EFI_MEMORY_WB; break;
        case 4: invalid.message.Length = 0; break;
        case 5: invalid.message.Length = 87; break;
        case 6: invalid.message.BaseAddress = 0xfffffffffffffff0ull; invalid.message.Length = 32; break;
        case 7: invalid.ipc.Length = 4; break;
        }
        const ProbeRpmGcd rejected = invalid.find();
        expect(rejected.status == EFI_UNSUPPORTED && !rejected.qualified,
               "RAM, reserved, nonexistent, cache-only, truncated or wrapping GCD resources cannot authorize native I/O");
    }
    for (unsigned failAt = 1; failAt <= 6; ++failAt) {
        Environment failed;
        failed.failAt = failAt;
        const ProbeRpmGcd rejected = failed.find();
        expect(rejected.status == EFI_DEVICE_ERROR && !rejected.qualified && failed.calls == failAt,
               "every GCD query failure stops qualification without continuing through untrusted output");
    }
    Environment changed;
    changed.changeSecond = true;
    expect(changed.find().status == EFI_COMPROMISED_DATA && changed.calls == 2,
           "inconsistent descriptors at the two allocation boundaries are refused");
    Environment wrapped;
    MemoryLayout malformed = layout;
    malformed.control.physical = ~static_cast<adreno::u64>(0) - 3;
    expect(ProbeFindRpmGcd(&wrapped.firmware, malformed).status == EFI_INVALID_PARAMETER && wrapped.calls == 0,
           "allocation overflow is refused before a GCD query");
    std::printf("EFI RPM GCD discovery: %u checks, %u failures (mock DXE; no resource-map writes)\n", checks, failures);
    return failures ? 1 : 0;
}
