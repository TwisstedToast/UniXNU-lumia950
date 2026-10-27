#include "GpuDiagnostic.h"
#include <cstdio>
#include <cstring>

namespace {
unsigned checks;
unsigned failures;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", message); }
}
}

int main()
{
    const uint32_t scans[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 0x17, 0x20, 0xff, 0x100,
        0xffff, 0x10000, 0xffffffff};
    for (uint32_t scan : scans) {
        const ProbeGpuMode expected = scan == 1 ? ProbeGpuMode::GpuOnly :
            scan == 2 ? ProbeGpuMode::SecureContexts : ProbeGpuMode::Cancelled;
        expect(ProbeGpuModeForScan(scan, true) == expected,
            "only the two explicit directional keys select a diagnostic mode");
        expect(ProbeGpuModeForScan(scan, false) == ProbeGpuMode::Cancelled,
            "an input error cancels even when a stale key contains a valid scan code");
        expect(ProbeGpuAllowsSecure(ProbeGpuModeForScan(scan, true)) == (scan == 2),
            "only explicit Volume Down selection authorizes native secure calls");
    }
    expect(!ProbeGpuAllowsSecure(ProbeGpuMode::GpuOnly) &&
        std::strcmp(ProbeGpuModeName(ProbeGpuMode::GpuOnly), "GPU_ONLY_NO_SECURE_CALLS") == 0,
        "the GPU-only mode is explicitly reported as having no secure calls");
    expect(ProbeGpuAllowsSecure(ProbeGpuMode::SecureContexts) &&
        std::strcmp(ProbeGpuModeName(ProbeGpuMode::SecureContexts), "GPU_AND_SECURE_CONTEXTS") == 0,
        "the secure-context mode has a distinct preflight record");
    const ProbeGpuMode invalid = static_cast<ProbeGpuMode>(0xffffffff);
    expect(!ProbeGpuAllowsSecure(invalid) && std::strcmp(ProbeGpuModeName(invalid), "CANCELLED") == 0,
        "an invalid mode never authorizes a secure call");
    const uint64_t results[] = {0, 1, 2, 0xffffffff, ~uint64_t(0)};
    for (uint64_t result : results) {
        expect(ProbeGpuScmResumePending(true, result) == (result == 1),
            "an interrupted monitor call must resume without diagnostic drawing or delay");
        expect(!ProbeGpuScmResumePending(false, result),
            "a trapped instruction is not mistaken for an interrupted secure-world call");
    }
    std::printf("GPU diagnostic selection: %u checks, %u failures (host policy, not handset evidence)\n",
        checks, failures);
    return failures ? 1 : 0;
}
