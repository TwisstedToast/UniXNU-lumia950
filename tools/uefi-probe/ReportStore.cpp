#include "ReportStore.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

namespace {
EFI_STATUS Status(ProbeReportResult *result, EFI_STATUS status)
{
    result->status = status;
    return status;
}

EFI_STATUS FindName(EFI_FILE_PROTOCOL *root, ProbeReportResult *result)
{
    const CHAR16 pattern[] = {'\\','E','F','I','\\','L','u','m','i','a','P','r','o','b','e','\\',
                              'g','p','u','-','r','e','p','o','r','t','-','0','0','.','t','x','t',0};
    for (UINTN index = 0; index < sizeof(pattern) / sizeof(pattern[0]); ++index) {
        result->filename[index] = pattern[index];
    }
    for (UINTN slot = 0; slot < 64; ++slot) {
        result->slot = slot;
        result->filename[27] = static_cast<CHAR16>('0' + slot / 10);
        result->filename[28] = static_cast<CHAR16>('0' + slot % 10);
        result->stage = ProbeReportStage::FindUnusedName;
        EFI_FILE_PROTOCOL *existing = nullptr;
        const EFI_STATUS status = root->Open(root, &existing, result->filename, EFI_FILE_MODE_READ, 0);
        if (status == EFI_NOT_FOUND) { return EFI_SUCCESS; }
        if (EFI_ERROR(status)) { return status; }
        if (!existing || !existing->Close) { return EFI_PROTOCOL_ERROR; }
        result->stage = ProbeReportStage::CloseExisting;
        const EFI_STATUS closed = existing->Close(existing);
        if (EFI_ERROR(closed)) { return closed; }
    }
    result->stage = ProbeReportStage::FindUnusedName;
    return EFI_VOLUME_FULL;
}

EFI_STATUS WriteFresh(EFI_FILE_PROTOCOL *root, const VOID *bytes, UINTN size,
                       ProbeReportResult *result)
{
    EFI_STATUS status = FindName(root, result);
    if (EFI_ERROR(status)) { return status; }
    result->stage = ProbeReportStage::CreateFile;
    EFI_FILE_PROTOCOL *file = nullptr;
    status = root->Open(root, &file, result->filename,
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status)) { return status; }
    if (!file || !file->Close) { return EFI_PROTOCOL_ERROR; }
    result->created = TRUE;
    if (!file->Write || !file->Flush) {
        result->fileCloseStatus = file->Close(file);
        return EFI_PROTOCOL_ERROR;
    }
    result->stage = ProbeReportStage::WriteFile;
    UINTN written = size;
    status = file->Write(file, &written, const_cast<VOID *>(bytes));
    if (!EFI_ERROR(status) && written != size) { status = EFI_DEVICE_ERROR; }
    if (!EFI_ERROR(status)) {
        result->stage = ProbeReportStage::FlushFile;
        status = file->Flush(file);
    }
    result->fileCloseStatus = file->Close(file);
    if (!EFI_ERROR(status) && EFI_ERROR(result->fileCloseStatus)) {
        result->stage = ProbeReportStage::CloseFile;
        status = result->fileCloseStatus;
    }
    return status;
}
}

EFI_STATUS ProbeSaveReport(EFI_BOOT_SERVICES *services, EFI_HANDLE image,
                           const VOID *bytes, UINTN size, ProbeReportResult *result)
{
    if (!result) { return EFI_INVALID_PARAMETER; }
    *result = {};
    result->stage = ProbeReportStage::Arguments;
    if (!services || !services->HandleProtocol || !image || !bytes || !size || size > 8192) {
        return Status(result, EFI_INVALID_PARAMETER);
    }
    result->stage = ProbeReportStage::LoadedImage;
    EFI_GUID loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = nullptr;
    EFI_STATUS status = services->HandleProtocol(image, &loadedGuid, reinterpret_cast<VOID **>(&loaded));
    if (EFI_ERROR(status)) { return Status(result, status); }
    if (!loaded) { return Status(result, EFI_PROTOCOL_ERROR); }
    result->stage = ProbeReportStage::LoadedDevice;
    if (!loaded->DeviceHandle) { return Status(result, EFI_NOT_FOUND); }
    result->stage = ProbeReportStage::Filesystem;
    EFI_GUID filesystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *filesystem = nullptr;
    status = services->HandleProtocol(loaded->DeviceHandle, &filesystemGuid,
                                     reinterpret_cast<VOID **>(&filesystem));
    if (EFI_ERROR(status)) { return Status(result, status); }
    if (!filesystem || !filesystem->OpenVolume) { return Status(result, EFI_PROTOCOL_ERROR); }
    result->stage = ProbeReportStage::OpenVolume;
    EFI_FILE_PROTOCOL *root = nullptr;
    status = filesystem->OpenVolume(filesystem, &root);
    if (EFI_ERROR(status)) { return Status(result, status); }
    if (!root || !root->Open || !root->Close) { return Status(result, EFI_PROTOCOL_ERROR); }
    status = WriteFresh(root, bytes, size, result);
    result->volumeCloseStatus = root->Close(root);
    if (!EFI_ERROR(status) && EFI_ERROR(result->volumeCloseStatus)) {
        result->stage = ProbeReportStage::CloseVolume;
        status = result->volumeCloseStatus;
    }
    if (!EFI_ERROR(status)) { result->stage = ProbeReportStage::Complete; }
    return Status(result, status);
}

const char *ProbeReportStageName(ProbeReportStage stage)
{
    switch (stage) {
    case ProbeReportStage::Arguments: return "ARGUMENTS";
    case ProbeReportStage::LoadedImage: return "LOADED_IMAGE";
    case ProbeReportStage::LoadedDevice: return "LOADED_DEVICE";
    case ProbeReportStage::Filesystem: return "FILESYSTEM";
    case ProbeReportStage::OpenVolume: return "OPEN_VOLUME";
    case ProbeReportStage::FindUnusedName: return "FIND_UNUSED_NAME";
    case ProbeReportStage::CloseExisting: return "CLOSE_EXISTING";
    case ProbeReportStage::CreateFile: return "CREATE_FILE";
    case ProbeReportStage::WriteFile: return "WRITE_FILE";
    case ProbeReportStage::FlushFile: return "FLUSH_FILE";
    case ProbeReportStage::CloseFile: return "CLOSE_FILE";
    case ProbeReportStage::CloseVolume: return "CLOSE_VOLUME";
    case ProbeReportStage::Complete: return "COMPLETE";
    }
    return "UNKNOWN";
}
