#include "FirmwareGcd.h"
#include <Guid/DxeServices.h>

namespace {
bool SameGuid(const EFI_GUID &first, const EFI_GUID &second)
{
    const auto *left = reinterpret_cast<const UINT8 *>(&first);
    const auto *right = reinterpret_cast<const UINT8 *>(&second);
    for (UINTN index = 0; index < sizeof(EFI_GUID); ++index) {
        if (left[index] != right[index]) { return false; }
    }
    return true;
}

bool Covers(const EFI_GCD_MEMORY_SPACE_DESCRIPTOR &descriptor, const adreno::rpm::MemoryRegion &region)
{
    const UINT64 maximum = ~static_cast<UINT64>(0);
    return descriptor.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo &&
        (descriptor.Capabilities & EFI_MEMORY_UC) && descriptor.Length &&
        descriptor.BaseAddress <= maximum - descriptor.Length &&
        descriptor.BaseAddress <= region.physical &&
        descriptor.BaseAddress + descriptor.Length >= region.physical + region.bytes;
}
}

ProbeRpmGcd ProbeFindRpmGcd(EFI_SYSTEM_TABLE *firmware, const adreno::rpm::MemoryLayout &layout)
{
    const adreno::rpm::MemoryRegion regions[] = {layout.control, layout.fifo, layout.doorbell};
    return ProbeFindMmioGcd(firmware, regions);
}

ProbeRpmGcd ProbeFindMmioGcd(EFI_SYSTEM_TABLE *firmware,
                            const adreno::rpm::MemoryRegion (&regions)[3])
{
    ProbeRpmGcd result = {};
    result.status = EFI_INVALID_PARAMETER;
    if (!firmware || !firmware->ConfigurationTable || !firmware->NumberOfTableEntries ||
        firmware->NumberOfTableEntries > 4096) { return result; }
    for (const auto &region : regions) {
        if (!region.physical || !region.bytes || region.physical > ~static_cast<UINT64>(0) - region.bytes) {
            return result;
        }
    }
    EFI_DXE_SERVICES *services = nullptr;
    const EFI_GUID expected = DXE_SERVICES_TABLE_GUID;
    for (UINTN index = 0; index < firmware->NumberOfTableEntries; ++index) {
        const auto &entry = firmware->ConfigurationTable[index];
        if (!SameGuid(entry.VendorGuid, expected)) { continue; }
        if (result.tablePresent) { result.status = EFI_COMPROMISED_DATA; return result; }
        result.tablePresent = TRUE;
        services = static_cast<EFI_DXE_SERVICES *>(entry.VendorTable);
    }
    result.status = EFI_NOT_FOUND;
    if (!result.tablePresent) { return result; }
    result.status = EFI_PROTOCOL_ERROR;
    const UINTN minimum = OFFSET_OF(EFI_DXE_SERVICES, GetMemorySpaceDescriptor) + sizeof(EFI_GET_MEMORY_SPACE_DESCRIPTOR);
    if (!services || (reinterpret_cast<UINTN>(services) & 7) ||
        services->Hdr.Signature != DXE_SERVICES_SIGNATURE || services->Hdr.HeaderSize < minimum ||
        services->Hdr.HeaderSize > 4096 || !services->GetMemorySpaceDescriptor) { return result; }
    for (UINT32 index = 0; index < 3; ++index) {
        auto &first = result.descriptors[index];
        result.status = services->GetMemorySpaceDescriptor(regions[index].physical, &first);
        if (EFI_ERROR(result.status)) { return result; }
        ++result.checkedRegions;
        if (!Covers(first, regions[index])) { result.status = EFI_UNSUPPORTED; return result; }
        EFI_GCD_MEMORY_SPACE_DESCRIPTOR last = {};
        result.status = services->GetMemorySpaceDescriptor(regions[index].physical + regions[index].bytes - 1, &last);
        if (EFI_ERROR(result.status)) { return result; }
        if (!Covers(last, regions[index]) || first.BaseAddress != last.BaseAddress ||
            first.Length != last.Length || first.Capabilities != last.Capabilities ||
            first.Attributes != last.Attributes || first.GcdMemoryType != last.GcdMemoryType) {
            result.status = EFI_COMPROMISED_DATA;
            return result;
        }
    }
    result.status = EFI_SUCCESS;
    result.qualified = TRUE;
    return result;
}
