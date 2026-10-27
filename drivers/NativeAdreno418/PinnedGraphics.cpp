#include "PinnedGraphics.h"

namespace adreno {
namespace pinned {

Result prepare(Handoff &handoff, spmi::Bus &bus, rpm::Session &session, bool exclusiveColdGpu)
{
    if (handoff.phase != Phase::Empty) { return Result::NotReady; }
    handoff.supply = spmi::Result::NotReady;
    handoff.clock = rpm::Result::NotReady;
    if (!exclusiveColdGpu || bus.state != spmi::State::Ready || session.state != rpm::State::Ready) {
        return Result::Unqualified;
    }
    handoff.phase = Phase::InspectSupply;
    handoff.supply = spmi::inspectGraphicsRail(bus, handoff.inherited, 10000);
    if (handoff.supply != spmi::Result::Ok) { return Result::SupplyFailed; }
    if (handoff.inherited.range || (handoff.inherited.mode & 0x3f) ||
        (handoff.inherited.enable & 0x7f)) {
        handoff.supply = spmi::Result::Unqualified;
        return Result::SupplyFailed;
    }
    handoff.phase = Phase::Clock;
    const rpm::Value rate = {rpm::kRateKey, kGraphicsKHz};
    handoff.clock = rpm::transact(session, rpm::Set::Active, rpm::kGraphicsParent, &rate, 1, 10000, 10);
    if (handoff.clock != rpm::Result::Ok) { return Result::ClockFailed; }
    handoff.phase = Phase::Supply;
    handoff.supply = spmi::enableGraphicsRail(bus, handoff.rail, true, 10000);
    if (handoff.supply != spmi::Result::Ok) { return Result::SupplyFailed; }
    handoff.phase = Phase::Settle;
    handoff.supply = spmi::verifyGraphicsRail(bus, handoff.rail, kSupplySamples, kSupplySampleDelayUs, 10000);
    if (handoff.supply != spmi::Result::Ok) { return Result::SupplyFailed; }
    handoff.phase = Phase::Ready;
    return Result::Ok;
}

const char *phaseName(Phase phase)
{
    switch (phase) {
    case Phase::Empty: return "GRAPHICS HANDOFF NOT STARTED";
    case Phase::InspectSupply: return "CHECKING GRAPHICS SUPPLY POLICY";
    case Phase::Clock: return "NATIVE GRAPHICS CLOCK REQUEST";
    case Phase::Supply: return "ESTABLISHING SUPPLY AFTER CLOCK ACK";
    case Phase::Settle: return "CHECKING FIXED SUPPLY FOR DRIFT";
    case Phase::Ready: return "FIXED CLOCK AND SUPPLY READY";
    }
    return "UNKNOWN GRAPHICS HANDOFF STAGE";
}

}
}
