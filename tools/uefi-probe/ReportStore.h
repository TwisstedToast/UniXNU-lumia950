#ifndef LUMIA_PROBE_REPORT_STORE_H
#define LUMIA_PROBE_REPORT_STORE_H

#include <Uefi.h>

enum class ProbeReportStage {
    Arguments, LoadedImage, LoadedDevice, Filesystem, OpenVolume,
    FindUnusedName, CloseExisting, CreateFile, WriteFile, FlushFile,
    CloseFile, CloseVolume, Complete
};

struct ProbeReportResult {
    ProbeReportStage stage;
    EFI_STATUS status;
    EFI_STATUS fileCloseStatus;
    EFI_STATUS volumeCloseStatus;
    UINTN slot;
    BOOLEAN created;
    CHAR16 filename[48];
};

EFI_STATUS ProbeSaveReport(EFI_BOOT_SERVICES *services, EFI_HANDLE image,
                           const VOID *bytes, UINTN size, ProbeReportResult *result);
const char *ProbeReportStageName(ProbeReportStage stage);

#endif
