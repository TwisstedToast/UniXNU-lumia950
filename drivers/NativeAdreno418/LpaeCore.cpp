#include "LpaeCore.h"

namespace adreno {
namespace lpae {
namespace {

bool rangeValid(u64 address, u64 bytes)
{
    return bytes != 0 && (address & (kPageBytes - 1)) == 0 &&
        (bytes & (kPageBytes - 1)) == 0 && address < kAddressLimit &&
        bytes <= kAddressLimit - address;
}

bool overlaps(u64 first, u64 firstBytes, u64 second, u64 secondBytes)
{
    return first < second + secondBytes && second < first + firstBytes;
}

u64 *tablePointer(const Tables &tables, u64 descriptor)
{
    if ((descriptor & 3) != 3 ||
        (descriptor & ~(kAddressMask | kNonSecureTable | 3ull)) != 0) {
        return nullptr;
    }
    const u64 physical = descriptor & kAddressMask;
    if (physical < tables.poolPhysical) { return nullptr; }
    const u64 offset = physical - tables.poolPhysical;
    if (offset / kPageBytes >= tables.usedPages) { return nullptr; }
    return tables.pool + offset / sizeof(u64);
}

u64 *allocateTable(Tables &tables, u64 *slot)
{
    if (tables.usedPages >= tables.poolPages) { return nullptr; }
    const u32 pageIndex = tables.usedPages++;
    u64 *result = tables.pool + static_cast<u64>(pageIndex) * kEntries;
    for (u32 index = 0; index < kEntries; index++) { result[index] = 0; }
    *slot = (tables.poolPhysical + static_cast<u64>(pageIndex) * kPageBytes) |
        kNonSecureTable | 3;
    return result;
}

u64 *pageSlot(const Tables &tables, u64 iova)
{
    u64 *middle = tablePointer(tables, tables.root[iova >> 30]);
    if (!middle) { return nullptr; }
    u64 *leaf = tablePointer(tables, middle[(iova >> 21) & 511]);
    return leaf ? &leaf[(iova >> 12) & 511] : nullptr;
}

bool preflight(const Tables &tables, u64 iova, u64 bytes, bool removing, u32 *needed)
{
    u32 missing = 0;
    u32 lastRoot = 0xffffffffu;
    u32 lastLeaf = 0xffffffffu;
    for (u64 address = iova; address < iova + bytes; address += kPageBytes) {
        const u32 rootIndex = static_cast<u32>(address >> 30);
        const u32 leafIndex = static_cast<u32>(address >> 21);
        const u64 rootEntry = tables.root[rootIndex];
        u64 *middle = rootEntry ? tablePointer(tables, rootEntry) : nullptr;
        if (rootEntry && !middle) { return false; }
        if (!middle && rootIndex != lastRoot) { missing++; }
        const u64 middleEntry = middle ? middle[(address >> 21) & 511] : 0;
        u64 *leaf = middleEntry ? tablePointer(tables, middleEntry) : nullptr;
        if (middleEntry && !leaf) { return false; }
        if (!leaf && leafIndex != lastLeaf) { missing++; }
        const u64 value = leaf ? leaf[(address >> 12) & 511] : 0;
        if (removing ? ((value & 3) != 3) : value != 0) { return false; }
        lastRoot = rootIndex;
        lastLeaf = leafIndex;
    }
    *needed = missing;
    return missing <= tables.poolPages - tables.usedPages;
}

}

bool validTables(const Tables &tables)
{
    const u64 poolBytes = static_cast<u64>(tables.poolPages) * kPageBytes;
    if (!tables.root || !tables.pool ||
        (reinterpret_cast<u64>(tables.root) & 7) != 0 ||
        (reinterpret_cast<u64>(tables.pool) & 7) != 0 ||
        !rangeValid(tables.rootPhysical, kPageBytes) ||
        !rangeValid(tables.poolPhysical, poolBytes) ||
        reinterpret_cast<u64>(tables.root) > ~0ull - kPageBytes ||
        reinterpret_cast<u64>(tables.pool) > ~0ull - poolBytes ||
        tables.usedPages > tables.poolPages) { return false; }
    return !overlaps(tables.rootPhysical, kPageBytes, tables.poolPhysical, poolBytes) &&
        !overlaps(reinterpret_cast<u64>(tables.root), kPageBytes,
                  reinterpret_cast<u64>(tables.pool), poolBytes);
}

bool mapRange(Tables &tables, u64 iova, u64 physical, u64 bytes,
              bool writable, bool cacheable)
{
    if (!validTables(tables) || !rangeValid(iova, bytes) || !rangeValid(physical, bytes)) {
        return false;
    }
    if (overlaps(physical, bytes, tables.rootPhysical, kPageBytes) ||
        overlaps(physical, bytes, tables.poolPhysical,
                 static_cast<u64>(tables.poolPages) * kPageBytes)) { return false; }
    u32 needed = 0;
    if (!preflight(tables, iova, bytes, false, &needed)) { return false; }
    const u64 attributes = 3 | kAccessFlag | kUnprivileged | kNonSecure |
        kExecuteNever | (writable ? 0 : kReadOnly) |
        (cacheable ? ((2ull << 2) | (3ull << 8)) : (1ull << 2));
    for (u64 offset = 0; offset < bytes; offset += kPageBytes) {
        const u64 address = iova + offset;
        u64 *rootSlot = &tables.root[address >> 30];
        u64 *middle = *rootSlot ? tablePointer(tables, *rootSlot) : allocateTable(tables, rootSlot);
        u64 *middleSlot = &middle[(address >> 21) & 511];
        u64 *leaf = *middleSlot ? tablePointer(tables, *middleSlot) : allocateTable(tables, middleSlot);
        leaf[(address >> 12) & 511] = (physical + offset) | attributes;
    }
    return true;
}

bool lookup(const Tables &tables, u64 iova, Mapping *mapping)
{
    if (!mapping || !validTables(tables) || iova >= kAddressLimit) { return false; }
    u64 *slot = pageSlot(tables, iova);
    if (!slot || (*slot & 3) != 3) { return false; }
    mapping->physical = (*slot & kAddressMask) | (iova & (kPageBytes - 1));
    mapping->writable = (*slot & kReadOnly) == 0;
    mapping->cacheable = ((*slot >> 2) & 7) == 2;
    return true;
}

bool matches(const Tables &tables, u64 iova, u64 physical, u64 bytes, bool writable)
{
    if (!rangeValid(iova, bytes) || !rangeValid(physical, bytes)) { return false; }
    for (u64 offset = 0; offset < bytes; offset += kPageBytes) {
        Mapping mapping = {};
        if (!lookup(tables, iova + offset, &mapping) || mapping.physical != physical + offset ||
            (writable && !mapping.writable)) { return false; }
    }
    return true;
}

bool unmapRange(Tables &tables, u64 iova, u64 bytes)
{
    if (!validTables(tables) || !rangeValid(iova, bytes)) { return false; }
    u32 needed = 0;
    if (!preflight(tables, iova, bytes, true, &needed)) { return false; }
    for (u64 offset = 0; offset < bytes; offset += kPageBytes) {
        *pageSlot(tables, iova + offset) = 0;
    }
    return true;
}

}
}
