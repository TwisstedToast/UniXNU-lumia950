#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/SmBios.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>

#include "ProbeCore.h"

static EFI_SYSTEM_TABLE *firmware_table;

void *memset(void *destination, int value, size_t byte_count)
{
    unsigned char *output = destination;
    for (size_t position = 0; position < byte_count; position++) {
        output[position] = (unsigned char)value;
    }
    return destination;
}

void *memcpy(void *destination, const void *source, size_t byte_count)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    for (size_t position = 0; position < byte_count; position++) {
        output[position] = input[position];
    }
    return destination;
}

static void PrintText(const CHAR16 *text)
{
    firmware_table->ConOut->OutputString(firmware_table->ConOut, (CHAR16 *)text);
}

static void PrintAscii(const char *text)
{
    CHAR16 buffer[80];
    size_t consumed = 0;

    while (text[consumed] != '\0') {
        size_t buffered = 0;
        while (buffered + 1 < sizeof(buffer) / sizeof(buffer[0]) &&
               text[consumed] != '\0') {
            buffer[buffered++] = (unsigned char)text[consumed++];
        }
        buffer[buffered] = 0;
        PrintText(buffer);
    }
}

static void PrintNumber(UINT64 value, unsigned int radix)
{
    char buffer[65];
    if (ProbeFormatUnsigned(value, radix, radix == 16 ? 16 : 1,
                            buffer, sizeof(buffer))) {
        PrintAscii(buffer);
    }
}

static void PrintDecimal(const CHAR16 *label, UINT64 value)
{
    PrintText(label);
    PrintNumber(value, 10);
    PrintText(L"\r\n");
}

static void PrintHex(const CHAR16 *label, UINT64 value)
{
    PrintText(label);
    PrintText(L"0x");
    PrintNumber(value, 16);
    PrintText(L"\r\n");
}

static int GuidEqual(const EFI_GUID *left, const EFI_GUID *right)
{
    const UINT8 *left_bytes = (const UINT8 *)left;
    const UINT8 *right_bytes = (const UINT8 *)right;
    for (size_t position = 0; position < sizeof(EFI_GUID); position++) {
        if (left_bytes[position] != right_bytes[position]) {
            return 0;
        }
    }
    return 1;
}

static void ReportGraphics(void)
{
    EFI_GUID graphics_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *graphics = NULL;
    EFI_STATUS status;

    status = firmware_table->BootServices->HandleProtocol(
        firmware_table->ConsoleOutHandle, &graphics_guid, (VOID **)&graphics);
    if (EFI_ERROR(status)) {
        status = firmware_table->BootServices->LocateProtocol(
            &graphics_guid, NULL, (VOID **)&graphics);
    }
    if (EFI_ERROR(status) || graphics == NULL || graphics->Mode == NULL ||
        graphics->Mode->Info == NULL) {
        PrintHex(L"GOP unavailable, status: ", status);
        return;
    }
    PrintText(L"GOP: ");
    PrintNumber(graphics->Mode->Info->HorizontalResolution, 10);
    PrintText(L"x");
    PrintNumber(graphics->Mode->Info->VerticalResolution, 10);
    PrintText(L" stride ");
    PrintNumber(graphics->Mode->Info->PixelsPerScanLine, 10);
    PrintText(L"\r\n");
    PrintHex(L"Framebuffer: ", graphics->Mode->FrameBufferBase);
    PrintText(L"FB bytes / pixel format: ");
    PrintNumber(graphics->Mode->FrameBufferSize, 10);
    PrintText(L" / ");
    PrintNumber(graphics->Mode->Info->PixelFormat, 10);
    PrintText(L"\r\n");
}

static enum ProbeMemoryKind MemoryKind(UINT32 memory_type)
{
    switch (memory_type) {
    case EfiConventionalMemory:
        return PROBE_MEMORY_FREE;
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        return PROBE_MEMORY_RECLAIMABLE;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
        return PROBE_MEMORY_RUNTIME;
    default:
        return PROBE_MEMORY_OTHER;
    }
}

static EFI_STATUS ReadMemorySummary(struct ProbeMemorySummary *summary)
{
    EFI_BOOT_SERVICES *services = firmware_table->BootServices;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    EFI_STATUS status = services->GetMemoryMap(
        &map_size, NULL, &map_key, &descriptor_size, &descriptor_version);

    if (status != EFI_BUFFER_TOO_SMALL) {
        return EFI_ERROR(status) ? status : EFI_COMPROMISED_DATA;
    }
    for (unsigned int attempt = 0; attempt < 5; attempt++) {
        UINTN capacity;
        if (descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
            descriptor_size > 4096 || map_size > 16 * 1024 * 1024) {
            return EFI_COMPROMISED_DATA;
        }
        capacity = map_size + descriptor_size * 8;
        status = services->AllocatePool(EfiLoaderData, capacity,
                                        (VOID **)&memory_map);
        if (EFI_ERROR(status)) {
            return status;
        }
        map_size = capacity;
        status = services->GetMemoryMap(&map_size, memory_map, &map_key,
                                        &descriptor_size, &descriptor_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            services->FreePool(memory_map);
            continue;
        }
        if (!EFI_ERROR(status) &&
            (descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
             descriptor_size % 8 != 0 || map_size == 0 || map_size > capacity ||
             map_size % descriptor_size != 0 ||
             descriptor_version != EFI_MEMORY_DESCRIPTOR_VERSION)) {
            status = EFI_COMPROMISED_DATA;
        }
        if (!EFI_ERROR(status)) {
            for (UINTN offset = 0; offset < map_size; offset += descriptor_size) {
                const EFI_MEMORY_DESCRIPTOR *descriptor =
                    (const EFI_MEMORY_DESCRIPTOR *)((const UINT8 *)memory_map + offset);
                if (!ProbeAddMemoryRange(summary, MemoryKind(descriptor->Type),
                                         descriptor->PhysicalStart,
                                         descriptor->NumberOfPages)) {
                    status = EFI_COMPROMISED_DATA;
                    break;
                }
            }
        }
        services->FreePool(memory_map);
        return status;
    }
    return EFI_BUFFER_TOO_SMALL;
}

static void ReportMemory(void)
{
    struct ProbeMemorySummary summary = {0};
    EFI_STATUS status = ReadMemorySummary(&summary);
    if (EFI_ERROR(status)) {
        PrintHex(L"Memory map unavailable, status: ", status);
        return;
    }
    PrintDecimal(L"Memory descriptors: ", summary.descriptor_count);
    PrintDecimal(L"Conventional RAM MiB: ",
                 summary.bytes_by_kind[PROBE_MEMORY_FREE] >> 20);
    PrintDecimal(L"Loader/boot-services RAM MiB: ",
                 summary.bytes_by_kind[PROBE_MEMORY_RECLAIMABLE] >> 20);
    PrintDecimal(L"Runtime RAM MiB (keep reserved): ",
                 summary.bytes_by_kind[PROBE_MEMORY_RUNTIME] >> 20);
    PrintHex(L"Largest free region base: ", summary.largest_free_base);
    PrintDecimal(L"Largest free region MiB: ", summary.largest_free_size >> 20);
}

static void ReportProtocolCount(const CHAR16 *label, EFI_GUID *protocol_guid)
{
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    EFI_STATUS status = firmware_table->BootServices->LocateHandleBuffer(
        ByProtocol, protocol_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) && status != EFI_NOT_FOUND) {
        PrintText(label);
        PrintHex(L"error ", status);
        return;
    }
    PrintDecimal(label, EFI_ERROR(status) ? 0 : handle_count);
    if (!EFI_ERROR(status)) {
        firmware_table->BootServices->FreePool(handles);
    }
}

static void ReportConfigurationTables(void)
{
    EFI_GUID acpi_guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID smbios_guid = SMBIOS_TABLE_GUID;
    EFI_GUID smbios3_guid = SMBIOS3_TABLE_GUID;
    for (UINTN index = 0; index < firmware_table->NumberOfTableEntries; index++) {
        const EFI_CONFIGURATION_TABLE *table = &firmware_table->ConfigurationTable[index];
        if (GuidEqual(&table->VendorGuid, &acpi_guid)) {
            PrintHex(L"ACPI 2 RSDP: ", (UINTN)table->VendorTable);
        } else if (GuidEqual(&table->VendorGuid, &smbios_guid) ||
                   GuidEqual(&table->VendorGuid, &smbios3_guid)) {
            PrintHex(L"SMBIOS entry: ", (UINTN)table->VendorTable);
        }
    }
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    UINT64 current_el;
    UINT64 cpu_id;
    UINT64 affinity;
    UINT64 timer_frequency;
    EFI_GUID filesystem_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID pointer_guid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;

    (void)image_handle;
    if (system_table == NULL || system_table->ConOut == NULL ||
        system_table->BootServices == NULL) {
        return EFI_UNSUPPORTED;
    }
    firmware_table = system_table;
    system_table->ConOut->ClearScreen(system_table->ConOut);
    PrintText(L"LUMIA 950 | UEFI BRING-UP PROBE v1\r\n");
    PrintText(L"Disk writes: NONE. No settings changes or EBS.\r\n");

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
    __asm__ volatile("mrs %0, MIDR_EL1" : "=r"(cpu_id));
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(affinity));
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(timer_frequency));
    PrintText(L"CurrentEL: EL");
    PrintNumber((current_el >> 2) & 3, 10);
    PrintText(((current_el >> 2) & 3) == 1
        ? L" | DarwinBoot EL1: OK\r\n"
        : L" | DarwinBoot EL1: NOT MET\r\n");
    PrintHex(L"MIDR_EL1: ", cpu_id);
    PrintHex(L"MPIDR_EL1: ", affinity);
    PrintDecimal(L"CNTFRQ Hz: ", timer_frequency);
    ReportGraphics();
    ReportMemory();
    ReportProtocolCount(L"Readable filesystem handles: ", &filesystem_guid);
    ReportProtocolCount(L"Block-I/O handles: ", &block_io_guid);
    ReportProtocolCount(L"Absolute-pointer handles: ", &pointer_guid);
    ReportConfigurationTables();

    PrintText(L"PROBE COMPLETE - photograph this screen.\r\n");
    PrintText(L"Press a key to return to the boot manager.\r\n");
    if (system_table->ConIn != NULL && system_table->ConIn->WaitForKey != NULL) {
        UINTN event_index = 0;
        EFI_INPUT_KEY input_key;
        EFI_STATUS status = system_table->BootServices->WaitForEvent(
            1, &system_table->ConIn->WaitForKey, &event_index);
        if (!EFI_ERROR(status)) {
            system_table->ConIn->ReadKeyStroke(system_table->ConIn, &input_key);
        }
    }
    PrintText(L"Returning to firmware.\r\n");
    return EFI_SUCCESS;
}
