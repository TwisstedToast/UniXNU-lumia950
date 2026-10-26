#include "PlatformCheck.h"

namespace adreno {
namespace {
u32 read32(const u8 *value)
{
    return static_cast<u32>(value[0]) | (static_cast<u32>(value[1]) << 8) |
        (static_cast<u32>(value[2]) << 16) | (static_cast<u32>(value[3]) << 24);
}
u64 read64(const u8 *value)
{
    return read32(value) | (static_cast<u64>(read32(value + 4)) << 32);
}
}

bool isLumia950Madt(const u8 *table, u32 bytes)
{
    if (!table || bytes < 44 || bytes > 65536 || read32(table + 4) != bytes ||
        read32(table) != 0x43495041 || read32(table + 36) != 0xf9002000) { return false; }
    u8 checksum = 0;
    for (u32 index = 0; index < bytes; index++) { checksum += table[index]; }
    if (checksum != 0) { return false; }
    const u64 affinities[] = {0, 1, 2, 3, 0x100, 0x101};
    u32 cpus = 0;
    u32 distributors = 0;
    for (u32 offset = 44; offset < bytes;) {
        if (bytes - offset < 2) { return false; }
        const u8 *entry = table + offset;
        const u32 length = entry[1];
        if (length < 2 || length > bytes - offset) { return false; }
        if (entry[0] == 11) {
            if (length < 80 || (read32(entry + 12) & 1) == 0 ||
                read64(entry + 32) != 0xf9002000) { return false; }
            const u64 affinity = read64(entry + 68);
            u32 matched = 0;
            for (u32 index = 0; index < 6; index++) {
                if (affinity == affinities[index]) { matched = 1u << index; }
            }
            if (matched == 0 || (cpus & matched) != 0) { return false; }
            cpus |= matched;
        } else if (entry[0] == 12) {
            if (length < 24 || read64(entry + 8) != 0xf9000000 || entry[20] != 2) {
                return false;
            }
            distributors++;
        }
        offset += length;
    }
    return cpus == 0x3f && distributors == 1;
}

}
