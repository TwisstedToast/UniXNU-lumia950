#ifndef LUMIA_PROBE_FIRMWARE_CLOCK_H
#define LUMIA_PROBE_FIRMWARE_CLOCK_H

#include <Uefi.h>

struct ProbeFirmwareClocks {
    EFI_STATUS status;
    BOOLEAN protocolPresent;
    BOOLEAN coreListed;
    BOOLEAN ahbListed;
    BOOLEAN timerListed;
};

ProbeFirmwareClocks ProbeFindFirmwareClocks(EFI_BOOT_SERVICES *services);

#endif
