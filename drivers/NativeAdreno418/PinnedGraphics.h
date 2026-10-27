#ifndef NATIVE_ADRENO418_PINNED_GRAPHICS_H
#define NATIVE_ADRENO418_PINNED_GRAPHICS_H

#include "RpmCore.h"
#include "SpmiCore.h"
#include "BoardConstants.h"

namespace adreno {
namespace pinned {

constexpr u32 kGraphicsKHz = lumia950::kBootGpuHz / 1000;
constexpr u32 kSupplySamples = 64;
constexpr u32 kSupplyAttempts = 256;
constexpr u32 kSupplySampleDelayUs = 250;

enum class Phase : u32 { Empty, InspectSupply, Clock, Settle, Ready };
enum class Result : u32 { Ok, NotReady, Unqualified, SupplyFailed, ClockFailed };
struct Handoff {
    Phase phase;
    spmi::RailSnapshot inherited;
    spmi::ObservedRail rail;
    spmi::Result supply;
    rpm::Result clock;
};

Result prepare(Handoff &handoff, spmi::Bus &bus, rpm::Session &session, bool exclusiveColdGpu);
const char *phaseName(Phase phase);

}
}
#endif
