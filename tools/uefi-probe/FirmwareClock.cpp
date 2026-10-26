#include "FirmwareClock.h"

namespace {
struct QcomClockLookup {
    VOID *(EFIAPI *get)(const CHAR8 *);
};
}

ProbeFirmwareClocks ProbeFindFirmwareClocks(EFI_BOOT_SERVICES *services)
{
    ProbeFirmwareClocks result = {};
    if (!services || !services->LocateProtocol) {
        result.status = EFI_INVALID_PARAMETER;
        return result;
    }
    EFI_GUID clockGuid = {0x4fcc91c2, 0x9c4f, 0x4e3c, {0xa6,0x73,0xc6,0xdf,0x62,0xe0,0x41,0xd5}};
    QcomClockLookup *clock = nullptr;
    result.status = services->LocateProtocol(&clockGuid, nullptr, reinterpret_cast<VOID **>(&clock));
    if (EFI_ERROR(result.status)) { return result; }
    if (!clock || !clock->get) {
        result.status = EFI_PROTOCOL_ERROR;
        return result;
    }
    result.protocolPresent = TRUE;
    result.coreListed = clock->get("oxili_gfx3d_clk") != nullptr;
    result.ahbListed = clock->get("oxilicx_ahb_clk") != nullptr;
    result.timerListed = clock->get("oxili_rbbmtimer_clk") != nullptr;
    return result;
}
