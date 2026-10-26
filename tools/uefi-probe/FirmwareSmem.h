#ifndef LUMIA_PROBE_FIRMWARE_SMEM_H
#define LUMIA_PROBE_FIRMWARE_SMEM_H

#include <Uefi.h>

enum class ProbeSmemStage : UINT32 {
    Arguments, LocateProtocol, Protocol, ReadTable, FindChannel, FindItems, ValidateLayout, Complete
};

struct ProbeFirmwareSmem {
    EFI_STATUS status;
    ProbeSmemStage stage;
    BOOLEAN protocolPresent;
    UINT32 channelCount;
    UINT32 channelId;
    UINT32 channelType;
    UINT32 referenceCount;
    UINT64 controlAddress;
    UINT64 fifoAddress;
    UINT32 controlBytes;
    UINT32 fifoBytes;
};

ProbeFirmwareSmem ProbeFindFirmwareSmem(EFI_BOOT_SERVICES *services);
const char *ProbeSmemStageName(ProbeSmemStage stage);

#endif
