#ifndef LUMIA_PROBE_FIRMWARE_MEMORY_H
#define LUMIA_PROBE_FIRMWARE_MEMORY_H

#include <Uefi.h>
#include "SmdMemory.h"

bool ProbeRpmMmioReserved(const VOID *map, UINTN mapBytes, UINTN stride,
                          const adreno::rpm::MemoryLayout &layout);

#endif
