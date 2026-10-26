#include "GpuIo.h"
#include "BoardConstants.h"

namespace adreno {

ProbeResult readPower(const RegisterIo &io, PowerSnapshot *snapshot)
{
    if (!snapshot || !io.read) { return ProbeResult::IoError; }
    *snapshot = {};
    if (!io.read(io.context, RegisterSpace::Mmss, lumia950::kGdscrGxOffset, &snapshot->gx) ||
        !io.read(io.context, RegisterSpace::Mmss, lumia950::kGdscrCxOffset, &snapshot->cx) ||
        !io.read(io.context, RegisterSpace::Mmss, kCoreClockOffset, &snapshot->coreClock) ||
        !io.read(io.context, RegisterSpace::Mmss, kAhbClockOffset, &snapshot->ahbClock) ||
        !io.read(io.context, RegisterSpace::Mmss, kTimerClockOffset, &snapshot->timerClock)) {
        return ProbeResult::IoError;
    }
    return checkPower(*snapshot);
}

ProbeResult checkPower(const PowerSnapshot &snapshot)
{
    const u32 samples[] = {snapshot.gx, snapshot.cx, snapshot.coreClock,
                           snapshot.ahbClock, snapshot.timerClock};
    for (u32 sample : samples) {
        if (sample == 0xffffffffu) { return ProbeResult::NoResponse; }
    }
    if ((snapshot.gx & lumia950::kGdscrPowerOn) == 0 ||
        (snapshot.cx & lumia950::kGdscrPowerOn) == 0) { return ProbeResult::DomainOff; }
    if (((snapshot.gx | snapshot.cx) & 1) != 0) { return ProbeResult::CollapsePending; }
    const u32 clocks[] = {snapshot.coreClock, snapshot.ahbClock, snapshot.timerClock};
    for (u32 sample : clocks) {
        if ((sample & kBranchEnable) == 0 || (sample & kBranchOff) != 0) {
            return ProbeResult::ClockOff;
        }
    }
    return ProbeResult::Ready;
}

ProbeResult readSnapshot(const RegisterIo &io, GpuSnapshot *snapshot)
{
    if (!snapshot || !io.read) { return ProbeResult::IoError; }
    *snapshot = {};
    const ProbeResult result = readPower(io, &snapshot->power);
    if (result != ProbeResult::Ready) { return result; }
    const u32 registers[] = {lumia950::kRegRbbmHwVersion,
        lumia950::kRegRbbmHwConfiguration, lumia950::kRegRbbmStatus,
        kInterruptStatusIndex, lumia950::kRegCpRbBase, lumia950::kRegCpRbCntl,
        lumia950::kRegCpRbRptr, lumia950::kRegCpRbWptr, lumia950::kRegCpMeCntl};
    u32 *outputs[] = {&snapshot->version, &snapshot->configuration, &snapshot->status,
        &snapshot->interruptStatus, &snapshot->ringBase, &snapshot->ringControl,
        &snapshot->readPointer, &snapshot->writePointer, &snapshot->meControl};
    for (u32 index = 0; index < sizeof(registers) / sizeof(registers[0]); index++) {
        if (!io.read(io.context, RegisterSpace::Gpu, registers[index] * 4, outputs[index])) {
            return ProbeResult::IoError;
        }
        if (*outputs[index] == 0xffffffffu || (index == 0 && *outputs[index] == 0)) {
            return ProbeResult::NoResponse;
        }
    }
    return ProbeResult::Ready;
}

const char *probeResultName(ProbeResult result)
{
    switch (result) {
    case ProbeResult::Ready: return "READY";
    case ProbeResult::IoError: return "MMIO_UNAVAILABLE";
    case ProbeResult::DomainOff: return "POWER_DOMAIN_OFF";
    case ProbeResult::CollapsePending: return "POWER_TRANSITION";
    case ProbeResult::ClockOff: return "CLOCK_OFF";
    case ProbeResult::NoResponse: return "NO_RESPONSE";
    }
    return "UNKNOWN";
}

}
