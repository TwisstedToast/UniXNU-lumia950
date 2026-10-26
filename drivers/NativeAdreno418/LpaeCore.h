#ifndef NATIVE_ADRENO418_LPAE_CORE_H
#define NATIVE_ADRENO418_LPAE_CORE_H

#include "AdrenoTypes.h"

namespace adreno {
namespace lpae {

constexpr u32 kPageBytes = 4096;
constexpr u32 kEntries = 512;
constexpr u64 kAddressLimit = 1ull << 32;
constexpr u64 kAddressMask = 0x00000000fffff000ull;
constexpr u64 kAccessFlag = 1ull << 10;
constexpr u64 kUnprivileged = 1ull << 6;
constexpr u64 kReadOnly = 1ull << 7;
constexpr u64 kExecuteNever = 3ull << 53;
constexpr u64 kNonSecure = 1ull << 5;
constexpr u64 kNonSecureTable = 1ull << 63;
constexpr u32 kMair0 = 0x00ff4404;
constexpr u32 kTcr = (1u << 31) | (1u << 23) | 32u;

struct Tables {
    u64 *root;
    u64 rootPhysical;
    u64 *pool;
    u64 poolPhysical;
    u32 poolPages;
    u32 usedPages;
};

struct Mapping {
    u64 physical;
    bool writable;
    bool cacheable;
};

bool validTables(const Tables &tables);
bool mapRange(Tables &tables, u64 iova, u64 physical, u64 bytes,
              bool writable, bool cacheable);
bool lookup(const Tables &tables, u64 iova, Mapping *mapping);
bool matches(const Tables &tables, u64 iova, u64 physical, u64 bytes, bool writable);
bool unmapRange(Tables &tables, u64 iova, u64 bytes);

}
}
#endif
