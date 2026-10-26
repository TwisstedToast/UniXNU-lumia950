// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NATIVE_SDHCI_H
#define NATIVE_SDHCI_H

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include "PollingHost.h"

// IOMemoryMap has no header of its own in the kernel tree: IOService.h declares the
// name for mapDeviceMemoryWithIndex(), and NativeSdhci.cpp includes the descriptor
// header for the complete class.

class NativeSdhci : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(NativeSdhci)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    // IOBlockStorageDevice
    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
    UInt32   doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    char    *getVendorString() override { return const_cast<char *>("Native"); }
    char    *getProductString() override { return const_cast<char *>("Lumia 950 eMMC"); }
    char    *getRevisionString() override { return const_cast<char *>("0.1"); }
    char    *getAdditionalDeviceInfoString() override { return const_cast<char *>("write path not implemented"); }
    IOReturn reportBlockSize(UInt64 *value) override;
    IOReturn reportEjectability(bool *value) override;
    IOReturn reportMaxValidBlock(UInt64 *value) override;
    IOReturn reportMediaState(bool *present, bool *changed) override;
    IOReturn reportRemovability(bool *value) override;
    IOReturn reportWriteProtection(bool *value) override;
    IOReturn doSynchronizeCache() override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                              IOStorageAttributes *attributes, IOStorageCompletion *completion) override;

private:
    // Register access. SDHCI mixes 8-, 16- and 32-bit registers, so the width is
    // explicit rather than derived from a 32-bit read.
    uint8_t  readHc8(uint32_t offset) const;
    uint16_t readHc16(uint32_t offset) const;
    uint32_t readHc32(uint32_t offset) const;
    void     writeHc8(uint32_t offset, uint8_t value);
    void     writeHc16(uint32_t offset, uint16_t value);
    void     writeHc32(uint32_t offset, uint32_t value);
    uint32_t readCore32(uint32_t offset) const;

    bool mapWindows(IOService *provider);
    void logHost();
    bool readBlockPio(uint64_t lba, void *bytes);
    bool probeCard();
    void logFailure();
    static sdhci::u8 ioRead8(void *context, sdhci::u32 offset);
    static sdhci::u32 ioRead32(void *context, sdhci::u32 offset);
    static void ioWrite8(void *context, sdhci::u32 offset, sdhci::u8 value);
    static void ioWrite16(void *context, sdhci::u32 offset, sdhci::u16 value);
    static void ioWrite32(void *context, sdhci::u32 offset, sdhci::u32 value);
    static void ioDelay(void *context, sdhci::u32 microseconds);

    IOLock            *gate = nullptr;
    volatile uint8_t  *hc = nullptr;
    volatile uint8_t  *core = nullptr;
    IOMemoryMap       *hcMap = nullptr;
    IOMemoryMap       *coreMap = nullptr;

    uint32_t baseClockHz = 0;
    uint16_t hostVersion = 0;
    uint16_t relativeAddress = 0;
    uint64_t capacityBytes = 0;
    uint64_t blockCount = 0;
    bool     online = false;
    bool     writeRefused = false;
    sdhci::PollingHost host;
    sdhci::WarmCard card;
};

#endif // NATIVE_SDHCI_H
