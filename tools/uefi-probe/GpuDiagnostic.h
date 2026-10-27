#ifndef LUMIA_GPU_DIAGNOSTIC_H
#define LUMIA_GPU_DIAGNOSTIC_H

#include <stdint.h>

enum class ProbeGpuMode : uint32_t { Cancelled, GpuOnly, SecureContexts };

constexpr ProbeGpuMode ProbeGpuModeForScan(uint32_t scan, bool readSucceeded)
{
    if (!readSucceeded) { return ProbeGpuMode::Cancelled; }
    return scan == 1 ? ProbeGpuMode::GpuOnly :
        scan == 2 ? ProbeGpuMode::SecureContexts : ProbeGpuMode::Cancelled;
}

constexpr bool ProbeGpuAllowsSecure(ProbeGpuMode mode)
{
    return mode == ProbeGpuMode::SecureContexts;
}

constexpr bool ProbeGpuScmResumePending(bool returned, uint64_t monitorResult)
{
    return returned && monitorResult == 1;
}

inline const char *ProbeGpuModeName(ProbeGpuMode mode)
{
    switch (mode) {
    case ProbeGpuMode::GpuOnly: return "GPU_ONLY_NO_SECURE_CALLS";
    case ProbeGpuMode::SecureContexts: return "GPU_AND_SECURE_CONTEXTS";
    case ProbeGpuMode::Cancelled: return "CANCELLED";
    }
    return "CANCELLED";
}

#endif
