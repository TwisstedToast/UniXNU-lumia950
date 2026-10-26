#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>

#include "FramebufferCore.h"
#include "ProbeCore.h"

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

static void FlushFramebuffer(const struct ProbeFramebuffer *framebuffer,
                              UINT32 first_row, UINT32 row_count)
{
    UINT64 cache_type;
    UINT64 line_size;
    UINT64 start = framebuffer->base + (UINT64)first_row * framebuffer->stride * 4;
    UINT64 end = start + (UINT64)row_count * framebuffer->stride * 4;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(cache_type));
    line_size = 4ULL << ((cache_type >> 16) & 15);
    for (UINT64 address = start & ~(line_size - 1); address < end; address += line_size) {
        __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void DrawStage(const struct ProbeFramebuffer *framebuffer,
                      const char *heading, UINT32 color)
{
    UINT32 scale = framebuffer->width >= 800 ? 4 : 2;
    UINT32 row_step = 12 * scale;
    ProbeFillRectangle(framebuffer, 0, 0, framebuffer->width, framebuffer->height, 0);
    ProbeFillRectangle(framebuffer, 0, 0, framebuffer->width, 12, color);
    ProbeDrawText(framebuffer, 24, 40, scale, heading, color);
    ProbeDrawText(framebuffer, 24, 40 + row_step, scale, "RAW FRAMEBUFFER", color);
    ProbeDrawText(framebuffer, 24, 40 + row_step * 2, scale, "MMU KEPT ON", color);
    ProbeDrawText(framebuffer, 24, 40 + row_step * 3, scale, "NO FIRMWARE CALLS AFTER EBS", color);
    ProbeDrawText(framebuffer, 24, 40 + row_step * 4, scale, "FORCE REBOOT TO EXIT", color);
    FlushFramebuffer(framebuffer, 0, framebuffer->height);
}

static _Noreturn void StopWithStatus(const struct ProbeFramebuffer *framebuffer,
                                     EFI_STATUS status)
{
    char status_text[17];
    UINT32 red = ProbeFramebufferColor(framebuffer->pixel_format, 255, 0, 0);
    __asm__ volatile("msr daifset, #15" ::: "memory");
    DrawStage(framebuffer, "HANDOFF FAILED", red);
    if (ProbeFormatUnsigned(status, 16, 16, status_text, sizeof(status_text))) {
        ProbeDrawText(framebuffer, 24, framebuffer->height / 2, 4, status_text, red);
        FlushFramebuffer(framebuffer, 0, framebuffer->height);
    }
    for (;;) {
        __asm__ volatile("wfe");
    }
}

static _Noreturn void AnimateAfterExit(const struct ProbeFramebuffer *framebuffer,
                                       UINT64 frequency)
{
    UINT32 green = ProbeFramebufferColor(framebuffer->pixel_format, 0, 255, 0);
    UINT32 bar_top = framebuffer->height / 2;
    UINT32 bar_height = 32;
    UINT32 block_width = framebuffer->width / 8;
    UINT32 slots = (framebuffer->width - block_width) / 24 + 1;
    UINT64 last_tick = UINT64_MAX;

    __asm__ volatile("msr daifset, #15" ::: "memory");
    DrawStage(framebuffer, "POST EBS OK", green);
    for (;;) {
        UINT64 counter;
        UINT64 tick;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(counter));
        tick = counter / (frequency / 8);
        if (tick != last_tick) {
            UINT32 left = (UINT32)(tick % slots) * 24;
            ProbeFillRectangle(framebuffer, 0, bar_top, framebuffer->width, bar_height, 0);
            ProbeFillRectangle(framebuffer, left, bar_top, block_width, bar_height, green);
            FlushFramebuffer(framebuffer, bar_top, bar_height);
            last_tick = tick;
        }
        __asm__ volatile("yield");
    }
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    EFI_GUID graphics_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *graphics = NULL;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_BOOT_SERVICES *services;
    EFI_GET_MEMORY_MAP get_memory_map;
    EFI_EXIT_BOOT_SERVICES exit_boot_services;
    struct ProbeFramebuffer framebuffer;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_size = 0;
    UINTN capacity;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    UINT64 current_el;
    UINT64 system_control;
    UINT64 frequency;
    EFI_STATUS status;
    EFI_INPUT_KEY input;
    UINTN event_index = 0;

    if (system_table == NULL || system_table->BootServices == NULL ||
        system_table->ConOut == NULL || system_table->ConIn == NULL ||
        system_table->ConIn->WaitForKey == NULL) {
        return EFI_UNSUPPORTED;
    }
    services = system_table->BootServices;
    system_table->ConOut->ClearScreen(system_table->ConOut);
    system_table->ConOut->OutputString(system_table->ConOut,
        L"LUMIA 950 | DISPLAY HANDOFF TEST\r\n"
        L"No disk writes or firmware-variable changes.\r\n"
        L"This test exits boot services and must be force-rebooted.\r\n");
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
    __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(system_control));
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(frequency));
    if (((current_el >> 2) & 3) != 1 || (system_control & 1) == 0 || frequency < 8) {
        system_table->ConOut->OutputString(system_table->ConOut,
            L"EL1, active MMU and an architectural timer are required.\r\n");
        return EFI_UNSUPPORTED;
    }
    status = services->HandleProtocol(system_table->ConsoleOutHandle,
                                      &graphics_guid, (VOID **)&graphics);
    if (EFI_ERROR(status)) {
        status = services->LocateProtocol(&graphics_guid, NULL, (VOID **)&graphics);
    }
    if (EFI_ERROR(status) || graphics == NULL || graphics->Mode == NULL ||
        graphics->Mode->Info == NULL) {
        return EFI_UNSUPPORTED;
    }
    framebuffer.base = graphics->Mode->FrameBufferBase;
    framebuffer.byte_size = graphics->Mode->FrameBufferSize;
    framebuffer.width = graphics->Mode->Info->HorizontalResolution;
    framebuffer.height = graphics->Mode->Info->VerticalResolution;
    framebuffer.stride = graphics->Mode->Info->PixelsPerScanLine;
    framebuffer.pixel_format = graphics->Mode->Info->PixelFormat;
    if (!ProbeFramebufferValid(&framebuffer) ||
        framebuffer.width < 320 || framebuffer.height < 320) {
        system_table->ConOut->OutputString(system_table->ConOut,
            L"A linear 32-bit RGB/BGR framebuffer is required.\r\n");
        return EFI_UNSUPPORTED;
    }
    status = services->HandleProtocol(image_handle, &loaded_image_guid,
                                      (VOID **)&loaded_image);
    if (EFI_ERROR(status) || loaded_image == NULL ||
        ProbeRangesOverlap(framebuffer.base, framebuffer.byte_size,
                           (UINTN)loaded_image->ImageBase, loaded_image->ImageSize)) {
        return EFI_COMPROMISED_DATA;
    }
    system_table->ConOut->OutputString(system_table->ConOut,
        L"HANDOFF READY - Volume Up starts test.\r\n"
        L"Any other key returns without exiting firmware.\r\n");
    for (;;) {
        status = services->WaitForEvent(1, &system_table->ConIn->WaitForKey, &event_index);
        if (EFI_ERROR(status)) {
            return status;
        }
        status = system_table->ConIn->ReadKeyStroke(system_table->ConIn, &input);
        if (status != EFI_NOT_READY) {
            break;
        }
    }
    if (EFI_ERROR(status) || input.ScanCode != SCAN_UP) {
        if (!EFI_ERROR(status)) {
            system_table->ConOut->OutputString(system_table->ConOut,
                L"HANDOFF CANCELLED - firmware retained.\r\n");
        }
        return EFI_ERROR(status) ? status : EFI_SUCCESS;
    }
    status = services->SetWatchdogTimer(0, 0, 0, NULL);
    if (EFI_ERROR(status)) {
        return status;
    }
    status = services->GetMemoryMap(&map_size, NULL, &map_key,
                                    &descriptor_size, &descriptor_version);
    if (status != EFI_BUFFER_TOO_SMALL || descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        descriptor_size > 4096 || map_size > 16 * 1024 * 1024) {
        return EFI_COMPROMISED_DATA;
    }
    capacity = map_size + descriptor_size * 64;
    status = services->AllocatePool(EfiLoaderData, capacity, (VOID **)&memory_map);
    if (EFI_ERROR(status)) {
        return status;
    }
    get_memory_map = services->GetMemoryMap;
    exit_boot_services = services->ExitBootServices;
    system_table->ConOut->OutputString(system_table->ConOut,
        L"Attempting ExitBootServices. Watch for POST EBS OK.\r\n");
    DrawStage(&framebuffer, "BEFORE EBS", ProbeFramebufferColor(
                  framebuffer.pixel_format, 255, 140, 0));
    services->Stall(1000000);
    for (unsigned int attempt = 0; attempt < 3; attempt++) {
        map_size = capacity;
        status = get_memory_map(&map_size, memory_map, &map_key,
                                 &descriptor_size, &descriptor_version);
        if (EFI_ERROR(status)) {
            if (attempt == 0) {
                services->FreePool(memory_map);
                return status;
            }
            StopWithStatus(&framebuffer, status);
        }
        status = exit_boot_services(image_handle, map_key);
        if (!EFI_ERROR(status)) {
            AnimateAfterExit(&framebuffer, frequency);
        }
        if (status != EFI_INVALID_PARAMETER) {
            StopWithStatus(&framebuffer, status);
        }
    }
    StopWithStatus(&framebuffer, status);
}
