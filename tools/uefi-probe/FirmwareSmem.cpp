#include "FirmwareSmem.h"

namespace {
constexpr UINT32 kChannels = 64;
constexpr UINT32 kWordsPerChannel = 8;
constexpr UINT32 kChannelTable = 13;
constexpr UINT32 kControlBase = 14;
constexpr UINT32 kFifoBase = 338;
constexpr UINT32 kAppsRpmType = 0x0f;

struct QcomSmemLookup {
    UINT32 (EFIAPI *read)(UINT32, VOID *, INT32);
    UINT32 (EFIAPI *readOffset)(UINT32, VOID *, INT32, INT32);
    VOID *(EFIAPI *get)(UINT32, UINT32 *);
};

bool ValidLayout(const ProbeFirmwareSmem &result)
{
    if (!result.controlAddress || !result.fifoAddress ||
        ((result.controlAddress | result.fifoAddress) & 3) || result.controlBytes < 88 ||
        result.fifoBytes < 1024 || result.fifoBytes > 131072 || (result.fifoBytes & 7)) {
        return false;
    }
    const UINT32 half = result.fifoBytes / 2;
    if ((half & (half - 1)) != 0) { return false; }
    const UINT64 maxAddress = ~static_cast<UINT64>(0);
    if (result.controlAddress > maxAddress - result.controlBytes ||
        result.fifoAddress > maxAddress - result.fifoBytes) { return false; }
    return result.controlAddress + result.controlBytes <= result.fifoAddress ||
        result.fifoAddress + result.fifoBytes <= result.controlAddress;
}
}

ProbeFirmwareSmem ProbeFindFirmwareSmem(EFI_BOOT_SERVICES *services)
{
    ProbeFirmwareSmem result = {};
    result.stage = ProbeSmemStage::Arguments;
    result.status = EFI_INVALID_PARAMETER;
    if (!services || !services->LocateProtocol) { return result; }

    result.stage = ProbeSmemStage::LocateProtocol;
    EFI_GUID guid = {0xe738ff9d, 0x020f, 0x4ecd, {0x87,0xfc,0xd5,0x82,0x50,0x75,0xaa,0xcd}};
    QcomSmemLookup *protocol = nullptr;
    result.status = services->LocateProtocol(&guid, nullptr, reinterpret_cast<VOID **>(&protocol));
    if (EFI_ERROR(result.status)) { return result; }
    result.stage = ProbeSmemStage::Protocol;
    if (!protocol || !protocol->read || !protocol->get) {
        result.status = EFI_PROTOCOL_ERROR;
        return result;
    }
    result.protocolPresent = TRUE;

    alignas(64) UINT32 table[kChannels * kWordsPerChannel] = {};
    result.stage = ProbeSmemStage::ReadTable;
    if (protocol->read(kChannelTable, table, sizeof(table)) != 0) {
        result.status = EFI_DEVICE_ERROR;
        return result;
    }
    result.stage = ProbeSmemStage::FindChannel;
    for (UINT32 index = 0; index < kChannels; ++index) {
        const UINT32 *entry = table + index * kWordsPerChannel;
        if ((entry[6] & 0xff) != kAppsRpmType) { continue; }
        ++result.channelCount;
        result.channelId = entry[5];
        result.channelType = entry[6];
        result.referenceCount = entry[7];
    }
    if (!result.channelCount) {
        result.status = EFI_NOT_FOUND;
        return result;
    }
    if (result.channelCount != 1 || result.channelId >= kChannels) {
        result.status = EFI_COMPROMISED_DATA;
        return result;
    }

    result.stage = ProbeSmemStage::FindItems;
    VOID *control = protocol->get(kControlBase + result.channelId, &result.controlBytes);
    VOID *fifo = protocol->get(kFifoBase + result.channelId, &result.fifoBytes);
    result.controlAddress = static_cast<UINT64>(reinterpret_cast<UINTN>(control));
    result.fifoAddress = static_cast<UINT64>(reinterpret_cast<UINTN>(fifo));
    if (!control || !fifo) {
        result.status = EFI_NOT_FOUND;
        return result;
    }
    result.stage = ProbeSmemStage::ValidateLayout;
    if (!ValidLayout(result)) {
        result.status = EFI_COMPROMISED_DATA;
        return result;
    }
    result.stage = ProbeSmemStage::Complete;
    result.status = EFI_SUCCESS;
    return result;
}

const char *ProbeSmemStageName(ProbeSmemStage stage)
{
    switch (stage) {
    case ProbeSmemStage::Arguments: return "ARGUMENTS";
    case ProbeSmemStage::LocateProtocol: return "LOCATE_PROTOCOL";
    case ProbeSmemStage::Protocol: return "PROTOCOL";
    case ProbeSmemStage::ReadTable: return "READ_TABLE";
    case ProbeSmemStage::FindChannel: return "FIND_CHANNEL";
    case ProbeSmemStage::FindItems: return "FIND_ITEMS";
    case ProbeSmemStage::ValidateLayout: return "VALIDATE_LAYOUT";
    case ProbeSmemStage::Complete: return "COMPLETE";
    }
    return "UNKNOWN";
}
