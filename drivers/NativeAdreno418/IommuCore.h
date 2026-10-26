// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The GPU's IOMMU page tables (rung 3, core only).
//
// MSM8992's graphics SMMU is a Qualcomm SMMU v1 (`qcom,msm-smmu-v1`,
// `qcom,iommu@fdb10000` in msm-iommu-v1.dtsi).
//
// The descriptor bit layout below is not recalled from the ARM ARM; it is the
// one drivers/iommu/io-pgtable-arm-v7s.c writes, mirrored field for field:
//
//   type          [1:0]  0b01 = coarse table pointer, 0b10 = page/section
//   table address [31:10] the coarse table is 1 KiB aligned
//   page address  [31:12] the page is 4 KiB aligned
//   ATTR_NG       bit 7 of the attribute field
//   ATTR_S        bit 6
//   TEX            bits [4:2] of the attribute field; 1 means normal,
//                  write-back cacheable
//   AP0 (AF)      bit 0 of the attribute field: access flag
//   AP1           bit 1: unprivileged access
//   AP2           bit 5: read-only
//   XN            BIT(4) in a section, BIT(0) in a page
//   B, C          BIT(2), BIT(3): outer/inner cacheable
//   NS            BIT(19) in a section, BIT(3) in a table pointer
//
// and the attribute field sits at bit 10 in a first-level entry and bit 4 in a
// second-level one, which is ARM_V7S_ATTR_SHIFT(lvl) = 16 - 6 * lvl.
//
// Every one of those is a function here so a test can pin the words. Nothing in
// this file allocates, maps or touches a register: the caller owns the tables
// and hands in their physical addresses, which is what the SMMU programs.
#ifndef NATIVE_ADRENO418_IOMMU_CORE_H
#define NATIVE_ADRENO418_IOMMU_CORE_H

#include "AdrenoTypes.h"

namespace adreno {
namespace smmu {

constexpr u32 kPageShift = 12;
constexpr u32 kPageBytes = 1u << kPageShift;          // 4096
constexpr u32 kSectionShift = 20;
constexpr u32 kSectionBytes = 1u << kSectionShift;    // 1 MiB

constexpr u32 kFirstLevelEntries = 4096;              // 1 << (32 - 20)
constexpr u32 kSecondLevelEntries = 256;              // 1 << (20 - 12)
constexpr u32 kTableBytes = kSecondLevelEntries * 4;  // 1 KiB
constexpr u32 kTableAlign = kTableBytes;

// Output addresses are 32 bits wide: a descriptor's last meaningful bit is 31.
constexpr u64 kAddressLimit = 1ull << 32;

constexpr u32 kTypeMask = 0x3;
constexpr u32 kTypeTable = 0x1;
constexpr u32 kTypePage = 0x2;
constexpr u32 kPageAddressMask = 0xfffff000u;
constexpr u32 kTableAddressMask = 0xfffffc00u;
constexpr u32 kSectionAddressMask = 0xfff00000u;

constexpr u32 kAttrNg = 1u << 7;
constexpr u32 kAttrS = 1u << 6;
constexpr u32 kAttrTexShift = 2;
constexpr u32 kAttrTexNormal = 1u;
constexpr u32 kAttrAp0 = 1u << 0;   // access flag
constexpr u32 kAttrAp1 = 1u << 1;   // unprivileged
constexpr u32 kAttrAp2 = 1u << 5;   // read-only
constexpr u32 kAttrB = 1u << 2;
constexpr u32 kAttrC = 1u << 3;
constexpr u32 kAttrNsSection = 1u << 19;
constexpr u32 kAttrNsTable = 1u << 3;

// The attribute field, already shifted into place for each level.
u32 attributeShift(u32 level);
u32 executeNeverBit(u32 level);

// A second-level (4 KiB) page descriptor. `nonSecure` marks the entry
// non-secure, which the GPU's context needs and the CPU's does not.
u32 pageDescriptor(u64 physical, bool cacheable, bool executeNever, bool nonSecure);

// A first-level 1 MiB section descriptor: one entry instead of a table, for a
// block the caller wants to map in one go.
u32 sectionDescriptor(u64 physical, bool cacheable, bool executeNever, bool nonSecure);

// A first-level pointer to a coarse second-level table.
u32 tableDescriptor(u64 tablePhysical, bool nonSecure);

u32 firstLevelIndex(u64 address);
u32 secondLevelIndex(u64 address);

// The two levels the caller owns. `secondLevel` is a pool of whole coarse tables;
// their physical addresses are computed from `secondLevelPhysical`, because that
// is what the SMMU reads.
struct Tables {
    u32 *firstLevel;              // kFirstLevelEntries entries, zeroed
    u32 *secondLevel;             // secondLevelCount * kSecondLevelEntries entries, zeroed
    u64  secondLevelPhysical;     // physical base of the pool, kTableAlign aligned
    u32  secondLevelCount;        // how many coarse tables the pool holds
    u32  secondLevelUsed;         // how many this encoder has written so far
    bool nonSecure;               // mark every descriptor non-secure
};

// Maps one 4 KiB page, allocating a coarse table the first time the 1 MiB it
// belongs to is touched. Returns false for an unaligned address, an address
// above the 32-bit window, a slot already holding a section, or an exhausted
// pool (in which case nothing is written).
bool mapPage(Tables &tables, u64 address, u64 physical, bool cacheable = true,
             bool executeNever = false);

// Maps one 1 MiB section at the first level. Returns false for an unaligned
// address, an address above the 32-bit window, or a slot that is already a page
// table.
bool mapSection(u32 *firstLevel, u64 address, u64 physical, bool cacheable = true,
                bool executeNever = false, bool nonSecure = false);

// Clears one page entry. Returns false when the page was not mapped.
bool unmapPage(Tables &tables, u64 address);

// How many pages a mapping of `bytes` at `address` needs, rounding the start
// down and the end up. Zero for an empty range.
u32 pagesFor(u64 address, u64 bytes);

} // namespace smmu
} // namespace adreno

#endif // NATIVE_ADRENO418_IOMMU_CORE_H
