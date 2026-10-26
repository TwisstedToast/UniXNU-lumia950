// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSdhci: the Lumia 950 (MSM8992) eMMC driver, bring-up rung 3.
//
// What this driver does today: takes over the controller the firmware left
// running, probes the card the firmware already selected, decodes its capacity,
// and serves 512-byte reads by programmed I/O with every wait polled. That is
// enough to read the GPT and prove the command path on real silicon.
//
// What it does not do yet, in the order the bring-up ladder adds it:
//   * interrupt delivery (hc_irq is a GICv2 SPI; ArmCpu's GIC path is the gate)
//   * ADMA2 transfers (buildAdma2 in SdhciCore.cpp is written and host-tested;
//     the descriptor registers are not wired up)
//   * writes (CMD24/CMD25, cache flush, and the eMMC power-on write protect)
//   * HS200/HS400 (QSDCC vendor registers; see README.md, "Vendor registers")
// Until then the medium reports itself read-only, which is honest rather than
// silently dropping writes.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>

#include "NativeSdhci.h"
#include "SdhciCore.h"
#include "SdhciRegs.h"
#include "BoardConstants.h"

using namespace sdhci;

OSDefineMetaClassAndStructors(NativeSdhci, IOBlockStorageDevice)

// Published for the bring-up log and the kernel symbol table, in the style of
// the other native drivers: a boot with no serial console still shows whether
// the driver started, how many blocks it moved and where it gave up.
extern "C" {
volatile uint32_t native_sdhci_ready  = 0;
volatile uint32_t native_sdhci_stage  = 0;   // last completed bring-up step
volatile uint32_t native_sdhci_rca    = 0;
volatile uint64_t native_sdhci_bytes  = 0;
volatile uint64_t native_sdhci_reads  = 0;
volatile uint64_t native_sdhci_errors = 0;
volatile uint32_t native_sdhci_error_phase = 0;
volatile uint32_t native_sdhci_interrupt_status = 0;
volatile uint32_t native_sdhci_card_status = 0;
}

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------

uint8_t NativeSdhci::readHc8(uint32_t offset) const
{
    if (!hc || offset >= lumia950::kSdhciHcSize) return 0xff;
    asm volatile("dsb sy" ::: "memory");
    const uint8_t value = hc[offset];
    asm volatile("dsb sy" ::: "memory");
    return value;
}

uint16_t NativeSdhci::readHc16(uint32_t offset) const
{
    if (!hc || (offset & 1) || offset > lumia950::kSdhciHcSize - 2) return 0xffff;
    asm volatile("dsb sy" ::: "memory");
    const uint16_t value = *reinterpret_cast<volatile uint16_t *>(hc + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

uint32_t NativeSdhci::readHc32(uint32_t offset) const
{
    if (!hc || (offset & 3) || offset > lumia950::kSdhciHcSize - 4) return 0xffffffffu;
    asm volatile("dsb sy" ::: "memory");
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(hc + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

void NativeSdhci::writeHc8(uint32_t offset, uint8_t value)
{
    if (!hc || offset >= lumia950::kSdhciHcSize) return;
    asm volatile("dsb sy" ::: "memory");
    hc[offset] = value;
    asm volatile("dsb sy" ::: "memory");
}

void NativeSdhci::writeHc16(uint32_t offset, uint16_t value)
{
    if (!hc || (offset & 1) || offset > lumia950::kSdhciHcSize - 2) return;
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint16_t *>(hc + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

void NativeSdhci::writeHc32(uint32_t offset, uint32_t value)
{
    if (!hc || (offset & 3) || offset > lumia950::kSdhciHcSize - 4) return;
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(hc + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

uint32_t NativeSdhci::readCore32(uint32_t offset) const
{
    if (!core || (offset & 3) || offset > lumia950::kSdhciCoreSize - 4) return 0xffffffffu;
    asm volatile("dsb sy" ::: "memory");
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(core + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

u8 NativeSdhci::ioRead8(void *context, u32 offset)
{
    return static_cast<NativeSdhci *>(context)->readHc8(offset);
}

u32 NativeSdhci::ioRead32(void *context, u32 offset)
{
    return static_cast<NativeSdhci *>(context)->readHc32(offset);
}

void NativeSdhci::ioWrite8(void *context, u32 offset, u8 value)
{
    static_cast<NativeSdhci *>(context)->writeHc8(offset, value);
}

void NativeSdhci::ioWrite16(void *context, u32 offset, u16 value)
{
    static_cast<NativeSdhci *>(context)->writeHc16(offset, value);
}

void NativeSdhci::ioWrite32(void *context, u32 offset, u32 value)
{
    static_cast<NativeSdhci *>(context)->writeHc32(offset, value);
}

void NativeSdhci::ioDelay(void *, u32 microseconds)
{
    IODelay(microseconds);
}

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

bool NativeSdhci::mapWindows(IOService *provider)
{
    // IOPlatformDevice publishes the ADT node's reg entries as device memory,
    // in order: 0 is the SDHCI window, 1 the Qualcomm core window.
    hcMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    coreMap = provider->mapDeviceMemoryWithIndex(1, kIOMapInhibitCache);
    if (!hcMap || !coreMap) {
        IOLog("NativeSdhci: provider has no reg windows (hc %p core %p); "
              "the ADT node needs both reg entries\n", hcMap, coreMap);
        return false;
    }
    if (hcMap->getLength() < lumia950::kSdhciHcSize) {
        IOLog("NativeSdhci: hc window is %llu bytes, expected at least 0x%x\n",
              static_cast<unsigned long long>(hcMap->getLength()), lumia950::kSdhciHcSize);
        return false;
    }

    if (coreMap->getLength() < lumia950::kSdhciCoreSize) {
        IOLog("NativeSdhci: core window is too short\n");
        return false;
    }

    hc = reinterpret_cast<volatile uint8_t *>(hcMap->getVirtualAddress());
    core = reinterpret_cast<volatile uint8_t *>(coreMap->getVirtualAddress());
    if (!hc || !core) {
        IOLog("NativeSdhci: window mapping returned no virtual address\n");
        return false;
    }
    return true;
}

void NativeSdhci::logHost()
{
    hostVersion = readHc16(kRegHostVersion);
    const uint32_t capabilities = readHc32(kRegCapabilities);
    baseClockHz = ((capabilities >> kCapBaseClockShift) & kCapBaseClockMask) * 1000000u;

    const uint16_t clockControl = readHc16(kRegClockControl);
    const uint32_t divider = decodeDivider(clockControl);
    const uint32_t cardHz = baseClockHz && divider ? baseClockHz / (2 * divider) : 0;

    IOLog("NativeSdhci: host version 0x%04x, capabilities 0x%08x\n", hostVersion, capabilities);
    IOLog("NativeSdhci: base clock %u Hz, clock control 0x%04x (divider %u, card %u Hz, enabled %u)\n",
          baseClockHz, clockControl, divider, cardHz, (clockControl & kClockSdClockEnable) ? 1 : 0);
    IOLog("NativeSdhci: present state 0x%08x, host control 0x%02x, power 0x%02x, "
          "core[0x00] 0x%08x\n",
          readHc32(kRegPresentState), readHc8(kRegHostControl1), readHc8(kRegPowerControl),
          readCore32(0x00));
}

void NativeSdhci::logFailure()
{
    const HostResult &failure = host.result();
    native_sdhci_error_phase = static_cast<uint32_t>(failure.phase);
    native_sdhci_interrupt_status = failure.interruptStatus;
    native_sdhci_card_status = failure.cardStatus;
    __atomic_fetch_add(&native_sdhci_errors, 1, __ATOMIC_RELAXED);
    IOLog("NativeSdhci: %s, CMD%u, phase %u, interrupt 0x%08x, card 0x%08x, attach %u\n",
          hostErrorName(failure.error), failure.command, static_cast<unsigned>(failure.phase),
          failure.interruptStatus, failure.cardStatus, static_cast<unsigned>(card.error()));
}

bool NativeSdhci::readBlockPio(uint64_t lba, void *bytes)
{
    if (card.readBlock(lba, static_cast<u8 *>(bytes))) return true;
    online = false;
    __atomic_store_n(&native_sdhci_ready, 0, __ATOMIC_RELAXED);
    logFailure();
    return false;
}

bool NativeSdhci::probeCard()
{
    const RegisterIo registers = {
        this, ioRead8, ioRead32, ioWrite8, ioWrite16, ioWrite32, ioDelay
    };
    if (!host.configure(registers) ||
        !card.attach(host, lumia950::kRcaCandidates,
                     sizeof lumia950::kRcaCandidates / sizeof lumia950::kRcaCandidates[0])) {
        logFailure();
        return false;
    }
    relativeAddress = card.info().relativeAddress;
    capacityBytes = card.info().capacityBytes;
    blockCount = capacityBytes / lumia950::kLogicalBlockBytes;
    __atomic_store_n(&native_sdhci_rca, relativeAddress, __ATOMIC_RELAXED);
    native_sdhci_stage = 3;
    IOLog("NativeSdhci: RCA %u, CSD %u, MMC %u, EXT_CSD %u, user partition, 512-byte sectors\n",
          relativeAddress, card.info().csd.structure, card.info().csd.mmcVersion,
          card.info().extCsd.revision);

    alignas(8) uint8_t gpt[lumia950::kLogicalBlockBytes] = {};
    if (!readBlockPio(lumia950::kGptHeaderLba, gpt)) return false;
    if (memcmp(gpt, lumia950::kGptSignature, sizeof lumia950::kGptSignature) != 0) {
        IOLog("NativeSdhci: GPT signature missing; not publishing the firmware medium\n");
        return false;
    }
    native_sdhci_stage = 4;
    IOLog("NativeSdhci: GPT signature read successfully\n");
    return true;
}

bool NativeSdhci::start(IOService *provider)
{
    if (!IOBlockStorageDevice::start(provider)) return false;

    gate = IOLockAlloc();
    if (!gate) {
        IOLog("NativeSdhci: no lock\n");
        return false;
    }
    if (!mapWindows(provider)) return false;

    IOLog("NativeSdhci: hc at %p (virtual), core at %p\n",
          reinterpret_cast<void *>(const_cast<uint8_t *>(hc)),
          reinterpret_cast<void *>(const_cast<uint8_t *>(core)));

    logHost();
    native_sdhci_stage = 0;

    if (!probeCard()) {
        IOLog("NativeSdhci: card probe failed; not publishing a medium\n");
        return false;
    }

    IOLockLock(gate);
    online = true;
    IOLockUnlock(gate);
    __atomic_store_n(&native_sdhci_ready, 1, __ATOMIC_RELAXED);

    IOLog("NativeSdhci: %llu blocks of %u bytes (%llu bytes), read-only until the "
          "write path exists\n",
          static_cast<unsigned long long>(blockCount), lumia950::kLogicalBlockBytes,
          static_cast<unsigned long long>(capacityBytes));
    registerService();
    return true;
}

void NativeSdhci::stop(IOService *provider)
{
    if (gate) {
        IOLockLock(gate);
        online = false;
        IOLockUnlock(gate);
    }
    card.detach();
    host.stop();
    __atomic_store_n(&native_sdhci_ready, 0, __ATOMIC_RELAXED);
    IOBlockStorageDevice::stop(provider);
}

void NativeSdhci::free()
{
    card.detach();
    host.stop();
    if (hcMap) { hcMap->release(); hcMap = nullptr; hc = nullptr; }
    if (coreMap) { coreMap->release(); coreMap = nullptr; core = nullptr; }
    if (gate) { IOLockFree(gate); gate = nullptr; }
    IOBlockStorageDevice::free();
}

// ---------------------------------------------------------------------------
// IOBlockStorageDevice
// ---------------------------------------------------------------------------

IOReturn NativeSdhci::reportBlockSize(UInt64 *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = lumia950::kLogicalBlockBytes;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportEjectability(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportMaxValidBlock(UInt64 *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = blockCount ? blockCount - 1 : 0;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportMediaState(bool *present, bool *changed)
{
    if (!present) return kIOReturnBadArgument;
    *present = online;
    if (changed) *changed = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportRemovability(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportWriteProtection(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    // The medium is not physically write protected; this driver has no write
    // path yet, so it reports the protection that is actually in force.
    *value = true;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                       IOStorageAttributes *, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count) return kIOReturnBadArgument;
    if (block >= blockCount || count > blockCount - block) return kIOReturnBadArgument;

    if (buffer->getDirection() == kIODirectionOut) {
        if (!writeRefused) {
            writeRefused = true;
            IOLog("NativeSdhci: refusing a write: the write path is not implemented "
                  "and the medium reports write protection\n");
        }
        return kIOReturnUnsupported;
    }
    if (buffer->getDirection() != kIODirectionIn) return kIOReturnBadArgument;

    if (count > ~static_cast<uint64_t>(0) / lumia950::kLogicalBlockBytes) return kIOReturnBadArgument;
    const uint64_t bytes = count * lumia950::kLogicalBlockBytes;
    if (buffer->getLength() < bytes) return kIOReturnBadArgument;

    IOLockLock(gate);
    if (!online) {
        IOLockUnlock(gate);
        return kIOReturnNotReady;
    }

    alignas(8) uint8_t staging[lumia950::kLogicalBlockBytes] = {};
    uint64_t done = 0;
    IOReturn status = kIOReturnSuccess;
    for (uint64_t index = 0; index < count; ++index) {
        if (!readBlockPio(block + index, staging)) {
            status = kIOReturnIOError;
            break;
        }
        const uint64_t offset = index * lumia950::kLogicalBlockBytes;
        if (buffer->writeBytes(offset, staging, lumia950::kLogicalBlockBytes) != lumia950::kLogicalBlockBytes) {
            status = kIOReturnIOError;
            break;
        }
        done += lumia950::kLogicalBlockBytes;
    }

    __atomic_fetch_add(&native_sdhci_bytes, done, __ATOMIC_RELAXED);
    if (done) __atomic_fetch_add(&native_sdhci_reads, 1, __ATOMIC_RELAXED);
    IOLockUnlock(gate);

    completion->action(completion->target, completion->parameter, status, done);
    return kIOReturnSuccess;
}
