// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NATIVE_ADRENO418_H
#define NATIVE_ADRENO418_H

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include "GpuIo.h"

// The Adreno 418 that MSM8992 carries is an a4xx part. Upstream's NativeAdreno
// drives a6xx/a7xx silicon through a GMU, a shared SMMU and a different command
// stream; none of that is a4xx, so this driver has its own class name instead of
// pretending to be a variant of the one it is not.
class NativeAdreno418 : public IOService {
    OSDeclareDefaultStructors(NativeAdreno418)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    // The ADT node's two windows: 0 is KGSL, 1 is the MMSS clock controller the
    // power domains report through.
    bool mapWindows(IOService *provider);

    uint32_t readMmss(uint32_t byteOffset) const;
    uint32_t readGpu(uint32_t dwordIndex) const;

    // Rung 1: prove the domains are on before touching the block, then read the
    // revision registers. Returns false when the driver must not go further.
    bool identify();
    static bool snapshotRead(void *, adreno::RegisterSpace, adreno::u32, adreno::u32 *);

    IOMemoryMap        *gpuMap  = nullptr;
    IOMemoryMap        *mmssMap = nullptr;
    volatile uint8_t   *gpu    = nullptr;
    volatile uint8_t   *mmss   = nullptr;

    // The two GDSCR values as last read, kept for a later rung that has to wait
    // on them; rung 1 only logs them.
    uint32_t gdscrGx = 0;
    uint32_t gdscrCx = 0;
};

#endif // NATIVE_ADRENO418_H
