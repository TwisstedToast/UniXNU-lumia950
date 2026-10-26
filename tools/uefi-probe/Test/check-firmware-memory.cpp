#include "../FirmwareMemory.h"
#include <array>
#include <cstdio>
#include <cstring>

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

struct Map {
    std::array<EFI_MEMORY_DESCRIPTOR, 3> entries = {};
    Map()
    {
        entries[0].Type = EfiMemoryMappedIO;
        entries[0].PhysicalStart = kMessageRamPhysical;
        entries[0].NumberOfPages = 8;
        entries[0].Attribute = EFI_MEMORY_UC;
        entries[1].Type = EfiMemoryMappedIO;
        entries[1].PhysicalStart = 0xf9000000;
        entries[1].NumberOfPages = 0x113;
        entries[1].Attribute = EFI_MEMORY_UC;
        entries[2].Type = EfiConventionalMemory;
        entries[2].PhysicalStart = 0x80000000;
        entries[2].NumberOfPages = 16;
        entries[2].Attribute = EFI_MEMORY_WB;
    }
    bool qualified() { return ProbeRpmMmioReserved(entries.data(), sizeof(entries), sizeof(entries[0]), layout); }
};
}

int main()
{
    Map map;
    expect(map.qualified(), "published RPM and IPC MMIO descriptors fully cover the measured allocations");
    for (UINTN stride : std::array<UINTN, 3>{sizeof(EFI_MEMORY_DESCRIPTOR), sizeof(EFI_MEMORY_DESCRIPTOR) + 8, 4096}) {
        std::array<UINT8, 12289> serialized = {};
        for (UINTN index = 0; index < 3; ++index) {
            std::memcpy(serialized.data() + 1 + index * stride, &map.entries[index], sizeof(EFI_MEMORY_DESCRIPTOR));
        }
        expect(ProbeRpmMmioReserved(serialized.data() + 1, stride * 3, stride, layout),
               "descriptor stride extensions and an unaligned host buffer are handled without undefined loads");
    }
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
        Map invalid;
        switch (mutation) {
        case 0: invalid.entries[0].Type = EfiConventionalMemory; break;
        case 1: invalid.entries[0].Type = EfiBootServicesData; break;
        case 2: invalid.entries[0].Type = EfiRuntimeServicesData; break;
        case 3: invalid.entries[0].Attribute = EFI_MEMORY_WB; break;
        case 4: invalid.entries[0].NumberOfPages = 0; break;
        case 5: invalid.entries[1].NumberOfPages = 1; break;
        case 6: invalid.entries[2] = invalid.entries[0]; break;
        case 7: invalid.entries[0].PhysicalStart += 1; break;
        case 8: invalid.entries[2].NumberOfPages = ~static_cast<UINT64>(0); break;
        case 9: invalid.entries[2].PhysicalStart = 0xfffffffffffff000ull; break;
        case 10: invalid.entries[2].PhysicalStart = kMessageRamPhysical; break;
        case 11: invalid.entries[1].PhysicalStart += 0xe000; break;
        }
        expect(!invalid.qualified(), "missing, overlapping, reclaimable or malformed reservation is refused");
    }
    for (UINTN stride : std::array<UINTN, 4>{0, 1, sizeof(EFI_MEMORY_DESCRIPTOR) - 1, 4097}) {
        expect(!ProbeRpmMmioReserved(map.entries.data(), sizeof(map.entries), stride, layout),
               "invalid memory-map stride is rejected before descriptor reads");
    }
    expect(!ProbeRpmMmioReserved(nullptr, sizeof(map.entries), sizeof(map.entries[0]), layout), "null map is refused");
    expect(!ProbeRpmMmioReserved(map.entries.data(), 0, sizeof(map.entries[0]), layout), "empty map is refused");
    expect(!ProbeRpmMmioReserved(map.entries.data(), sizeof(map.entries) - 1, sizeof(map.entries[0]), layout),
           "partial final descriptor is refused");
    expect(!ProbeRpmMmioReserved(map.entries.data(), 32 * 1024 * 1024, sizeof(map.entries[0]), layout),
           "oversized map is rejected before any descriptor load");
    MemoryLayout wrapped = layout;
    wrapped.fifo.physical = ~static_cast<adreno::u64>(0) - 3;
    expect(!ProbeRpmMmioReserved(map.entries.data(), sizeof(map.entries), sizeof(map.entries[0]), wrapped),
           "allocation range overflow cannot appear covered");
    expect(ProbeRpmMmioReserved(&map.entries[2], sizeof(map.entries[2]), sizeof(map.entries[2]), layout, true),
           "separately qualified GCD MMIO need not be listed in the UEFI RAM map");
    expect(!ProbeRpmMmioReserved(&map.entries[2], sizeof(map.entries[2]), sizeof(map.entries[2]), layout),
           "a missing UEFI MMIO descriptor still refuses without independently qualified GCD coverage");
    for (UINT32 type : std::array<UINT32, 7>{EfiConventionalMemory, EfiLoaderCode, EfiLoaderData,
            EfiBootServicesCode, EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData}) {
        Map conflict;
        conflict.entries[0].Type = type;
        expect(!ProbeRpmMmioReserved(conflict.entries.data(), sizeof(conflict.entries), sizeof(conflict.entries[0]), layout, true),
               "GCD coverage cannot override UEFI RAM, loader, boot-services or runtime allocation conflicts");
    }
    Map noAttributes;
    noAttributes.entries[0].Attribute = 0;
    expect(ProbeRpmMmioReserved(noAttributes.entries.data(), sizeof(noAttributes.entries), sizeof(noAttributes.entries[0]), layout, true),
           "GCD UC capabilities plus actual Device translations qualify MMIO with omitted UEFI attributes");
    Map reserved;
    reserved.entries[0].Type = EfiReservedMemoryType;
    expect(ProbeRpmMmioReserved(reserved.entries.data(), sizeof(reserved.entries), sizeof(reserved.entries[0]), layout, true),
           "GCD-classified MMIO may also be marked reserved by the UEFI map");
    Map duplicate;
    duplicate.entries[2] = duplicate.entries[0];
    expect(!ProbeRpmMmioReserved(duplicate.entries.data(), sizeof(duplicate.entries), sizeof(duplicate.entries[0]), layout, true),
           "GCD does not excuse duplicate overlapping UEFI descriptors");
    Map empty;
    empty.entries[0].NumberOfPages = 0;
    expect(!ProbeRpmMmioReserved(empty.entries.data(), sizeof(empty.entries), sizeof(empty.entries[0]), layout, true),
           "GCD does not excuse a malformed zero-page UEFI descriptor");
    std::printf("EFI RPM reservations: %u checks, %u failures (synthetic memory maps)\n", checks, failures);
    return failures ? 1 : 0;
}
