// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeAdreno418: the Lumia 950 (MSM8992) GPU driver, bring-up rung 1.
//
// The SoC's GPU is an Adreno 418, an a4xx part: no GMU, a 4 KiB IOMMU, the
// a4xx command processor and IR3 shaders. This driver does the one thing that
// can be done honestly before any of that: it maps the register window, asks
// the clock controller whether the firmware left the graphics power domains on,
// and only then reads the revision registers. A collapsed domain is not read at
// all, because an AHB access to a powered-down block is how a bring-up hangs the
// interconnect instead of reporting a failure.
//
// It publishes no GPU service. Nothing that looks for a Metal device may find
// this one: a client that binds a half driver is worse than a client that finds
// none and renders on the CPU.
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

#include "NativeAdreno418.h"
#include "AdrenoCore.h"
#include "BoardConstants.h"

using namespace adreno;

OSDefineMetaClassAndStructors(NativeAdreno418, IOService)

// Published for the bring-up log and the kernel symbol table, as NativeSdhci
// does: a boot with no serial console still shows how far the GPU got.
extern "C" {
volatile uint32_t native_adreno418_stage        = 0;   // last completed rung
volatile uint32_t native_adreno418_chipid       = 0;   // as the device tree states it
volatile uint32_t native_adreno418_gdscr_gx     = 0;
volatile uint32_t native_adreno418_gdscr_cx     = 0;
volatile uint32_t native_adreno418_hw_version   = 0;
volatile uint32_t native_adreno418_window_alive = 0;   // the window answered a read
volatile uint32_t native_adreno418_power_polls   = 0;   // reads spent waiting on a GDSC
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

bool NativeAdreno418::mapWindows(IOService *provider)
{
    // IOPlatformDevice publishes the ADT node's reg entries as device memory in
    // order: 0 is the KGSL window, 1 the MMSS clock controller holding the GDSCs.
    gpuMap  = provider->mapDeviceMemoryWithIndex(0, 0);
    mmssMap = provider->mapDeviceMemoryWithIndex(1, 0);
    if (!gpuMap || !mmssMap) {
        IOLog("NativeAdreno418: provider has no reg windows (kgsl %p mmss %p); "
              "the ADT node needs both\n", gpuMap, mmssMap);
        return false;
    }
    if (gpuMap->getLength() < lumia950::kGpuSize) {
        IOLog("NativeAdreno418: KGSL window is %llu bytes, expected at least 0x%x\n",
              static_cast<unsigned long long>(gpuMap->getLength()), lumia950::kGpuSize);
        return false;
    }
    if (mmssMap->getLength() < lumia950::kMmssSize) {
        IOLog("NativeAdreno418: MMSS window is %llu bytes, expected at least 0x%x\n",
              static_cast<unsigned long long>(mmssMap->getLength()), lumia950::kMmssSize);
        return false;
    }

    gpu  = reinterpret_cast<volatile uint8_t *>(gpuMap->getVirtualAddress());
    mmss = reinterpret_cast<volatile uint8_t *>(mmssMap->getVirtualAddress());
    if (!gpu || !mmss) {
        IOLog("NativeAdreno418: window mapping returned no virtual address\n");
        return false;
    }
    return true;
}

uint32_t NativeAdreno418::readMmss(uint32_t offset) const
{
    if (!mmss) { return 0; }
    if ((offset & 3) || offset > lumia950::kMmssSize - 4) { return 0xffffffffu; }
    asm volatile("dsb sy" ::: "memory");
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(mmss + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

uint32_t NativeAdreno418::readGpu(uint32_t dwordIndex) const
{
    if (!gpu) { return 0xffffffffu; }
    if (dwordIndex >= lumia950::kGpuSize / 4) { return 0xffffffffu; }
    asm volatile("dsb sy" ::: "memory");
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(gpu + byteOffset(dwordIndex));
    asm volatile("dsb sy" ::: "memory");
    return value;
}

// ---------------------------------------------------------------------------
// Rung 1: identity
// ---------------------------------------------------------------------------

bool NativeAdreno418::snapshotRead(void *context, RegisterSpace space, u32 offset, u32 *value)
{
    if (!context || !value || (offset & 3)) { return false; }
    NativeAdreno418 *driver = static_cast<NativeAdreno418 *>(context);
    if (space == RegisterSpace::Mmss && offset <= lumia950::kMmssSize - 4) {
        *value = driver->readMmss(offset);
        return true;
    }
    if (space == RegisterSpace::Gpu && offset <= lumia950::kGpuSize - 4) {
        *value = driver->readGpu(offset / 4);
        return true;
    }
    return false;
}

bool NativeAdreno418::identify()
{
    const RegisterIo io = {this, snapshotRead, nullptr, nullptr, nullptr};
    GpuSnapshot snapshot = {};
    const ProbeResult result = readSnapshot(io, &snapshot);
    gdscrGx = snapshot.power.gx;
    gdscrCx = snapshot.power.cx;
    native_adreno418_gdscr_gx = gdscrGx;
    native_adreno418_gdscr_cx = gdscrCx;

    IOLog("NativeAdreno418: %s; GX 0x%08x CX 0x%08x clocks core/AHB/timer "
          "0x%08x/0x%08x/0x%08x; no power or clock writes\n", probeResultName(result),
          gdscrGx, gdscrCx, snapshot.power.coreClock, snapshot.power.ahbClock,
          snapshot.power.timerClock);
    if (result != ProbeResult::Ready) { return false; }
    const uint32_t hwVersion = snapshot.version;
    const uint32_t hwConfiguration = snapshot.configuration;
    const uint32_t status = snapshot.status;
    native_adreno418_hw_version = hwVersion;

    if (classifyWindowRead(hwVersion) == WindowAnswer::NoResponse) {
        IOLog("NativeAdreno418: KGSL window answered 0xffffffff, so nothing is backing "
              "the address the ADT gives; check it against the firmware before blaming "
              "the driver\n");
        return false;
    }
    native_adreno418_window_alive = 1;

    const ChipIdentity expected = decodeChipId(lumia950::kChipId);
    native_adreno418_chipid = lumia950::kChipId;
    IOLog("NativeAdreno418: device tree chipid 0x%08x is generation %u, %u.%u.%u; "
          "RBBM_HW_VERSION 0x%08x, RBBM_HW_CONFIGURATION 0x%08x, RBBM_STATUS 0x%08x\n",
          lumia950::kChipId, expected.generation, expected.major, expected.minor,
          expected.patch, hwVersion, hwConfiguration, status);

    if (!isA4xx(expected)) {
        IOLog("NativeAdreno418: generation %u is not a4xx; this driver's registers and "
              "command stream are a4xx only, so it stops here\n", expected.generation);
        return false;
    }
    if (!plausibleVersion(hwVersion)) {
        // Worth saying out loud: this read proves the window is backed, not which
        // revision answered. The revision table is a hardware-documentation
        // question, not something to infer from a value that looks wrong.
        IOLog("NativeAdreno418: the version read proves the window is backed; it is not "
              "the revision this driver expects, and no revision is claimed from it\n");
    }

    const uint32_t rbBase  = snapshot.ringBase;
    const uint32_t rbCntl  = snapshot.ringControl;
    const uint32_t rbRptr  = snapshot.readPointer;
    const uint32_t rbWptr  = snapshot.writePointer;
    const uint32_t meCntl  = snapshot.meControl;
    IOLog("NativeAdreno418: CP left by the firmware: RB_BASE 0x%08x RB_CNTL 0x%08x "
          "RB_RPTR 0x%08x RB_WPTR 0x%08x ME_CNTL 0x%08x\n",
          rbBase, rbCntl, rbRptr, rbWptr, meCntl);

    native_adreno418_stage = static_cast<uint32_t>(Rung::Identity);
    return true;
}

// ---------------------------------------------------------------------------
// IOService
// ---------------------------------------------------------------------------

bool NativeAdreno418::start(IOService *provider)
{
    if (!IOService::start(provider)) { return false; }
    if (!mapWindows(provider)) { return false; }

    IOLog("NativeAdreno418: kgsl at %p (virtual), mmss at %p\n",
          reinterpret_cast<void *>(const_cast<uint8_t *>(gpu)),
          reinterpret_cast<void *>(const_cast<uint8_t *>(mmss)));

    if (!identify()) {
        IOLog("NativeAdreno418: rung 1 of %u (%s) did not complete; the GPU stays "
              "unowned\n", static_cast<unsigned>(Rung::Metal), rungName(Rung::Identity));
        return false;
    }

    IOLog("NativeAdreno418: rung 1 of %u (%s) is the furthest this build has: "
          "the SMMU and CP backend is not activated; cold power and Metal remain "
          "unimplemented. No GPU service is published.\n",
          static_cast<unsigned>(Rung::Metal), rungName(kWrittenRung));
    return true;
}

void NativeAdreno418::stop(IOService *provider)
{
    IOService::stop(provider);
}

void NativeAdreno418::free()
{
    if (gpuMap)  { gpuMap->release();  gpuMap = nullptr;  gpu = nullptr; }
    if (mmssMap) { mmssMap->release(); mmssMap = nullptr; mmss = nullptr; }
    IOService::free();
}
