#ifndef LUMIA_PROBE_FIRMWARE_GCD_H
#define LUMIA_PROBE_FIRMWARE_GCD_H

#include <PiDxe.h>
#include "SmdMemory.h"

struct ProbeRpmGcd {
    EFI_STATUS status;
    BOOLEAN tablePresent;
    BOOLEAN qualified;
    UINT32 checkedRegions;
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR descriptors[3];
};

ProbeRpmGcd ProbeFindRpmGcd(EFI_SYSTEM_TABLE *firmware, const adreno::rpm::MemoryLayout &layout);

#endif
