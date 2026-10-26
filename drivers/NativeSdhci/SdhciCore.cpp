// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdhciCore.h"
#include "SdhciRegs.h"

namespace sdhci {

bool decodeCsd(const u32 csd[4], CsdInfo *out)
{
    if (!csd || !out) return false;

    const u32 structure   = csd[0] >> 30;
    const u32 readBlockLen = (csd[1] >> 16) & 0xF;
    const u32 cSize        = ((csd[1] & 0x3FF) << 2) | (csd[2] >> 30);
    const u32 cSizeMult    = (csd[2] >> 15) & 0x7;

    if (!structure || readBlockLen < 9 || readBlockLen > 11) return false;

    CsdInfo info = {};
    info.structure    = structure;
    info.mmcVersion   = (csd[0] >> 26) & 0xf;
    info.readBlockLen = readBlockLen;
    info.cSize        = cSize;
    info.cSizeMult    = cSizeMult;

    info.saturated = info.mmcVersion >= 4 && cSize == 0xfff;
    const u64 blocks = static_cast<u64>(cSize) + 1;
    const u64 mult = static_cast<u64>(1) << (cSizeMult + 2);
    const u64 length = static_cast<u64>(1) << readBlockLen;
    info.capacityBytes = info.saturated ? 0 : blocks * mult * length;

    *out = info;
    return true;
}

void normalizeResponse136(const u32 registers[4], u32 response[4])
{
    if (!registers || !response) return;
    u32 normalized[4];
    for (u32 word = 0; word < 4; ++word) {
        normalized[word] = registers[3 - word] << 8;
        if (word != 3) normalized[word] |= registers[2 - word] >> 24;
    }
    for (u32 word = 0; word < 4; ++word) response[word] = normalized[word];
}

bool decodeExtCsd(const u8 *bytes, u32 length, ExtCsdInfo *out)
{
    if (!bytes || length != 512 || !out || bytes[194] > 2 || bytes[61] > 1) return false;
    ExtCsdInfo info = {};
    info.capacityBytes = parseExtCsdSectorCount(bytes);
    info.sectorBytes = bytes[61] ? 4096 : 512;
    info.revision = bytes[192];
    info.partitionAccess = bytes[179] & 7;
    info.commandQueueEnabled = (bytes[15] & 1) != 0;
    *out = info;
    return true;
}

u64 parseExtCsdSectorCount(const u8 extCsd[512])
{
    if (!extCsd) return 0;
    const u32 sectors = static_cast<u32>(extCsd[212]) |
                             (static_cast<u32>(extCsd[213]) << 8) |
                             (static_cast<u32>(extCsd[214]) << 16) |
                             (static_cast<u32>(extCsd[215]) << 24);
    return static_cast<u64>(sectors) * 512;
}

u32 clockDivider(u32 baseHz, u32 targetHz)
{
    if (!baseHz || !targetHz) return 0;

    // The card clock is base / (2 * divider), so round the divider up to keep
    // the result at or below the target.
    const u64 denominator = static_cast<u64>(targetHz) * 2;
    u64 divider = (static_cast<u64>(baseHz) + denominator - 1) / denominator;
    if (divider < 1) divider = 1;
    if (divider > 1023) return 0;
    return static_cast<u32>(divider);
}

u16 encodeDivider(u32 divider)
{
    divider &= 0x3FF;
    return static_cast<u16>((((divider >> 8) & 0x3) << 6) | ((divider & 0xFF) << 8));
}

u32 decodeDivider(u16 clockControl)
{
    return ((static_cast<u32>(clockControl) >> 6) & 0x3) << 8 |
           ((static_cast<u32>(clockControl) >> 8) & 0xFF);
}

u32 buildAdma2(const Adma2Chunk *chunks, u32 count, u64 *table, u32 tableEntries)
{
    if (!chunks || !count || !table || count > tableEntries) return 0;

    for (u32 index = 0; index < count; ++index) {
        const Adma2Chunk &chunk = chunks[index];
        if (!chunk.length || chunk.length > 0x10000 || (chunk.length & 3)) return 0;
        if (chunk.phys & 3) return 0;
        // ADMA2 descriptors carry a 32-bit address; the Lumia eMMC controller
        // has no SMMU context, so every buffer must be reachable below 4 GiB.
        if (chunk.phys > 0xFFFFFFFFULL) return 0;

        u16 attributes = kAdma2AttrValid | kAdma2AttrActTransfer;
        if (index + 1 == count) attributes |= kAdma2AttrEnd;

        // A length of 65536 is encoded as zero, which the controller reads as
        // the full 64 KiB.
        const u64 length = chunk.length & 0xFFFF;
        table[index] = static_cast<u64>(attributes) | (length << 16) | (chunk.phys << 32);
    }
    return count;
}

u64 maxValidLba(u64 capacityBytes)
{
    return capacityBytes >= 512 ? capacityBytes / 512 - 1 : 0;
}

} // namespace sdhci
