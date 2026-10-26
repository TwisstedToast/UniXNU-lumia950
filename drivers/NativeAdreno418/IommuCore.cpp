// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "IommuCore.h"

namespace adreno {
namespace smmu {

u32 attributeShift(u32 level)
{
    // io-pgtable-arm-v7s.c: ARM_V7S_ATTR_SHIFT(lvl) = 16 - 6 * lvl.
    return level == 1 ? 10u : 4u;
}

u32 executeNeverBit(u32 level)
{
    // ARM_V7S_ATTR_XN(lvl) = BIT(4 * (2 - lvl)): bit 4 in a section, bit 0 in a page.
    return level == 1 ? (1u << 4) : (1u << 0);
}

namespace {

// The attribute field both levels share, before it is shifted into place. The
// kernel sets NG and S unconditionally, TEX(1) for normal memory, and (with
// permissions enabled) the access flag plus unprivileged access.
u32 attributes(bool readOnly)
{
    u32 value = kAttrNg | kAttrS | (kAttrTexNormal << kAttrTexShift);
    value |= kAttrAp0 | kAttrAp1;            // access flag, unprivileged
    if (readOnly) { value |= kAttrAp2; }
    return value;
}

bool withinWindow(u64 address)
{
    // Both the input address and the output address are 32 bits wide here: the
    // format has no room for more, and the SMMU's registers hold 32-bit bases.
    return address < kAddressLimit;
}

} // namespace

u32 pageDescriptor(u64 physical, bool cacheable, bool executeNever, bool nonSecure)
{
    if (!withinWindow(physical)) { return 0; }
    u32 pte = attributes(false) << attributeShift(2);
    if (cacheable) { pte |= kAttrB | kAttrC; }
    if (executeNever) { pte |= executeNeverBit(2); }
    pte |= static_cast<u32>(physical) & kPageAddressMask;
    pte |= kTypePage;
    // The section NS bit is a first-level bit; a page entry carries no NS flag of
    // its own in this format, so `nonSecure` only matters to the table and
    // section descriptors. It stays in the signature so callers do not have to
    // know that, and so the day it does matter the call sites do not change.
    (void)nonSecure;
    return pte;
}

u32 sectionDescriptor(u64 physical, bool cacheable, bool executeNever, bool nonSecure)
{
    if (!withinWindow(physical)) { return 0; }
    u32 pte = attributes(false) << attributeShift(1);
    if (cacheable) { pte |= kAttrB | kAttrC; }
    if (executeNever) { pte |= executeNeverBit(1); }
    pte |= static_cast<u32>(physical) & kSectionAddressMask;
    pte |= kTypePage;
    if (nonSecure) { pte |= kAttrNsSection; }
    return pte;
}

u32 tableDescriptor(u64 tablePhysical, bool nonSecure)
{
    if (!withinWindow(tablePhysical)) { return 0; }
    u32 pte = static_cast<u32>(tablePhysical) & kTableAddressMask;
    pte |= kTypeTable;
    if (nonSecure) { pte |= kAttrNsTable; }
    return pte;
}

u32 firstLevelIndex(u64 address)
{
    return static_cast<u32>((address >> kSectionShift) & (kFirstLevelEntries - 1));
}

u32 secondLevelIndex(u64 address)
{
    return static_cast<u32>((address >> kPageShift) & (kSecondLevelEntries - 1));
}

bool mapPage(Tables &tables, u64 address, u64 physical, bool cacheable, bool executeNever)
{
    if (!tables.firstLevel || !tables.secondLevel ||
        tables.secondLevelUsed > tables.secondLevelCount ||
        (tables.secondLevelPhysical & (kTableAlign - 1)) != 0 ||
        tables.secondLevelPhysical >= kAddressLimit ||
        static_cast<u64>(tables.secondLevelCount) * kTableBytes >
        kAddressLimit - tables.secondLevelPhysical) { return false; }
    if (!withinWindow(address) || !withinWindow(physical)) { return false; }
    if ((address & (kPageBytes - 1)) || (physical & (kPageBytes - 1))) { return false; }

    const u32 first = firstLevelIndex(address);
    u32 *entry = &tables.firstLevel[first];

    u32 *table = nullptr;
    if (*entry == 0) {
        if (tables.secondLevelUsed >= tables.secondLevelCount) { return false; }
        const u64 tablePhysical = tables.secondLevelPhysical
                                + static_cast<u64>(tables.secondLevelUsed) * kTableBytes;
        const u32 descriptor = tableDescriptor(tablePhysical, tables.nonSecure);
        if (descriptor == 0) { return false; }
        table = &tables.secondLevel[static_cast<u64>(tables.secondLevelUsed) * kSecondLevelEntries];
        tables.secondLevelUsed++;
        *entry = descriptor;
    } else if ((*entry & kTypeMask) == kTypeTable) {
        // Recover which pool table the entry points at, so a mapping written
        // against an existing table lands in that table and not in a new one.
        const u64 tablePhysical = static_cast<u64>(*entry & kTableAddressMask);
        if (tablePhysical < tables.secondLevelPhysical) { return false; }
        const u64 offset = tablePhysical - tables.secondLevelPhysical;
        if (offset % kTableBytes) { return false; }
        const u64 index = offset / kTableBytes;
        if (index >= tables.secondLevelUsed) { return false; }
        table = &tables.secondLevel[index * kSecondLevelEntries];
    } else {
        // A section already covers this megabyte; replacing it would drop a
        // mapping this encoder did not make.
        return false;
    }

    const u32 second = secondLevelIndex(address);
    if (table[second] != 0) { return false; }
    table[second] = pageDescriptor(physical, cacheable, executeNever, tables.nonSecure);
    return table[second] != 0;
}

bool mapSection(u32 *firstLevel, u64 address, u64 physical, bool cacheable,
                bool executeNever, bool nonSecure)
{
    if (!firstLevel) { return false; }
    if (!withinWindow(address) || !withinWindow(physical)) { return false; }
    if ((address & (kSectionBytes - 1)) || (physical & (kSectionBytes - 1))) { return false; }

    const u32 first = firstLevelIndex(address);
    if (firstLevel[first] != 0) { return false; }
    const u32 descriptor = sectionDescriptor(physical, cacheable, executeNever, nonSecure);
    if (descriptor == 0) { return false; }
    firstLevel[first] = descriptor;
    return true;
}

bool unmapPage(Tables &tables, u64 address)
{
    if (!tables.firstLevel || !tables.secondLevel) { return false; }
    if (!withinWindow(address) || (address & (kPageBytes - 1)) != 0 ||
        tables.secondLevelUsed > tables.secondLevelCount) { return false; }

    u32 *entry = &tables.firstLevel[firstLevelIndex(address)];
    if ((*entry & kTypeMask) != kTypeTable) { return false; }
    const u64 tablePhysical = static_cast<u64>(*entry & kTableAddressMask);
    if (tablePhysical < tables.secondLevelPhysical) { return false; }
    const u64 offset = tablePhysical - tables.secondLevelPhysical;
    if (offset % kTableBytes) { return false; }
    const u64 index = offset / kTableBytes;
    if (index >= tables.secondLevelUsed) { return false; }

    u32 &slot = tables.secondLevel[index * kSecondLevelEntries + secondLevelIndex(address)];
    if (slot == 0) { return false; }
    slot = 0;
    return true;
}

u32 pagesFor(u64 address, u64 bytes)
{
    if (bytes == 0 || address >= kAddressLimit || bytes > kAddressLimit - address) { return 0; }
    const u64 start = address & ~static_cast<u64>(kPageBytes - 1);
    const u64 end = address + bytes;
    if (end < address) { return 0; }                 // wrapped: not a range
    const u64 pages = ((end + kPageBytes - 1) & ~static_cast<u64>(kPageBytes - 1)) - start;
    return static_cast<u32>(pages / kPageBytes);
}

} // namespace smmu
} // namespace adreno
