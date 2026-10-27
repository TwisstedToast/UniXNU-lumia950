#include "FirmwareMemory.h"

namespace {
bool Covered(const UINT8 *map, UINTN mapBytes, UINTN stride, const adreno::rpm::MemoryRegion &region,
             bool gcdQualified)
{
    if (!region.physical || !region.bytes ||
        region.physical > ~static_cast<UINT64>(0) - region.bytes) { return false; }
    const UINT64 end = region.physical + region.bytes;
    UINT32 covering = 0;
    for (UINTN offset = 0; offset < mapBytes; offset += stride) {
        EFI_MEMORY_DESCRIPTOR descriptor = {};
        const UINT8 *source = map + offset;
        auto *destination = reinterpret_cast<UINT8 *>(&descriptor);
        for (UINTN index = 0; index < sizeof(descriptor); ++index) { destination[index] = source[index]; }
        if (!descriptor.NumberOfPages) { return false; }
        if (descriptor.NumberOfPages > (~static_cast<UINT64>(0) >> 12)) { return false; }
        const UINT64 bytes = descriptor.NumberOfPages << 12;
        if (descriptor.PhysicalStart > ~static_cast<UINT64>(0) - bytes ||
            (descriptor.PhysicalStart & 4095)) { return false; }
        const UINT64 descriptorEnd = descriptor.PhysicalStart + bytes;
        if (region.physical >= descriptorEnd || descriptor.PhysicalStart >= end) { continue; }
        if ((descriptor.Type != EfiMemoryMappedIO && !(gcdQualified && descriptor.Type == EfiReservedMemoryType)) ||
            (!gcdQualified && !(descriptor.Attribute & EFI_MEMORY_UC)) ||
            region.physical < descriptor.PhysicalStart || end > descriptorEnd) { return false; }
        ++covering;
    }
    return covering == 1 || (!covering && gcdQualified);
}
}

bool ProbeRpmMmioReserved(const VOID *map, UINTN mapBytes, UINTN stride,
                          const adreno::rpm::MemoryLayout &layout, bool gcdQualified)
{
    const adreno::rpm::MemoryRegion regions[] = {layout.control, layout.fifo, layout.doorbell};
    return ProbeMmioRangesReserved(map, mapBytes, stride, regions, 3, gcdQualified);
}

bool ProbeMmioRangesReserved(const VOID *map, UINTN mapBytes, UINTN stride,
    const adreno::rpm::MemoryRegion *regions, UINTN count, bool gcdQualified)
{
    if (!map || !mapBytes || mapBytes > 16 * 1024 * 1024 ||
        stride < sizeof(EFI_MEMORY_DESCRIPTOR) || stride > 4096 || mapBytes % stride ||
        !regions || !count || count > 16) { return false; }
    const auto *bytes = static_cast<const UINT8 *>(map);
    for (UINTN index = 0; index < count; ++index) {
        if (!Covered(bytes, mapBytes, stride, regions[index], gcdQualified)) { return false; }
    }
    return true;
}
