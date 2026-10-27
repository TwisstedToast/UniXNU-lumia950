#include "SpmiCore.h"
#include "BoardConstants.h"

namespace adreno {
namespace spmi {
namespace {

Result fail(Bus &bus, Result result)
{
    bus.state = State::Poisoned;
    return result;
}

Result transfer(Bus &bus, u8 slave, u16 address, bool writing, u8 &value, u32 &remaining)
{
    if (bus.state != State::Ready) { return Result::NotReady; }
    if (!remaining) { return fail(bus, Result::TimedOut); }
    u32 configuration = 0;
    if (!bus.io.read(bus.io.context, kConfigurationOffset, &configuration)) { return fail(bus, Result::IoError); }
    if (configuration) { return fail(bus, Result::Unqualified); }
    bus.command = (writing ? 0u : 1u << 27) | (static_cast<u32>(slave) << 20) |
        (static_cast<u32>(address >> 8) << 12) | (static_cast<u32>(address & 255) << 4);
    bus.state = State::InFlight;
    if (writing && !bus.io.write(bus.io.context, kWriteDataOffset, value)) { return fail(bus, Result::IoError); }
    bus.io.barrier(bus.io.context);
    if (!bus.io.write(bus.io.context, kCommandOffset, bus.command)) { return fail(bus, Result::IoError); }
    bus.io.barrier(bus.io.context);
    while (remaining) {
        --remaining;
        if (!bus.io.read(bus.io.context, kStatusOffset, &bus.status)) { return fail(bus, Result::IoError); }
        if (bus.status) {
            if (bus.status != 1) {
                return fail(bus, (bus.status & 4) ? Result::Denied : Result::Failed);
            }
            bus.io.barrier(bus.io.context);
            if (!writing) {
                u32 data = 0;
                if (!bus.io.read(bus.io.context, kReadDataOffset, &data)) { return fail(bus, Result::IoError); }
                value = static_cast<u8>(data & 255);
            }
            ++bus.transfers;
            bus.state = State::Ready;
            return Result::Ok;
        }
        if (remaining) { bus.io.delayUs(bus.io.context, 1); }
    }
    return fail(bus, Result::TimedOut);
}

bool voltage(RailSnapshot &snapshot)
{
    if (snapshot.type != 0x1c || snapshot.subtype != 9 || snapshot.range > 1) { return false; }
    snapshot.microvolts = snapshot.range == 0 ? 80000u + 5000u * snapshot.voltage :
        160000u + 10000u * snapshot.voltage;
    return snapshot.range == 0 ? snapshot.microvolts >= 350000 && snapshot.microvolts <= 1355000 :
        snapshot.microvolts >= 1360000 && snapshot.microvolts <= 2200000;
}

bool ready(const RailSnapshot &snapshot)
{
    return snapshot.microvolts >= lumia950::kFixedGpuMicrovolts &&
        snapshot.microvolts <= lumia950::kMaximumGpuMicrovolts &&
        snapshot.range == 0 && (snapshot.enable & 0x80) &&
        (snapshot.status & 0x80) && (snapshot.mode & 0x80);
}

Result inspect(Bus &bus, RailSnapshot &snapshot, u32 &remaining)
{
    snapshot = {};
    const u16 addresses[] = {0x1704, 0x1705, 0x1708, 0x1740, 0x1741, 0x1745, 0x1746};
    u8 *values[] = {&snapshot.type, &snapshot.subtype, &snapshot.status, &snapshot.range,
                   &snapshot.voltage, &snapshot.mode, &snapshot.enable};
    for (u32 index = 0; index < 7; ++index) {
        const Result result = transfer(bus, 3, addresses[index], false, *values[index], remaining);
        if (result != Result::Ok) { return result; }
    }
    return voltage(snapshot) ? Result::Ok : Result::Unqualified;
}

bool observedReady(const RailSnapshot &snapshot, bool rpmManaged)
{
    return knownGraphicsPolicy(snapshot) && snapshot.microvolts >=
        (rpmManaged ? 350000u : lumia950::kFixedGpuMicrovolts) &&
        snapshot.microvolts <= lumia950::kMaximumGpuMicrovolts && snapshot.enable == 0x80 &&
        (snapshot.status & 0x80) && (snapshot.mode == 0x40 || snapshot.mode == 0x80);
}

Result observe(Bus &bus, ObservedRail &rail, u32 &remaining)
{
    const Result result = inspect(bus, rail.final, remaining);
    if (result != Result::Ok) { return result; }
    if (!rail.samples) { rail.initial = rail.final; }
    ++rail.samples;
    const u32 microvolts = rail.final.microvolts;
    if (!rail.minimumMicrovolts || microvolts < rail.minimumMicrovolts) { rail.minimumMicrovolts = microvolts; }
    if (microvolts > rail.maximumMicrovolts) { rail.maximumMicrovolts = microvolts; }
    return knownGraphicsPolicy(rail.final) ? Result::Ok : Result::Unqualified;
}

}

Result bind(Bus &bus, const Io &io, bool firmwareExited, bool exclusiveChannel)
{
    if (bus.state != State::Empty) { return Result::NotReady; }
    if (!io.read || !io.write || !io.barrier || !io.delayUs) { return Result::Invalid; }
    if (!firmwareExited || !exclusiveChannel) { return Result::Unqualified; }
    bus.io = io;
    u32 configuration = 0;
    if (!io.read(io.context, kVersionOffset, &bus.version) ||
        !io.read(io.context, kConfigurationOffset, &configuration)) { return fail(bus, Result::IoError); }
    if (!bus.version || bus.version >= 0x20010000 || configuration) { return fail(bus, Result::Unqualified); }
    bus.state = State::Ready;
    return Result::Ok;
}

Result readByte(Bus &bus, u8 slave, u16 address, u8 &value, u32 polls)
{
    if (slave > 15 || !polls || polls > 100000) { return Result::Invalid; }
    return transfer(bus, slave, address, false, value, polls);
}

Result writeGraphicsByte(Bus &bus, u16 address, u8 value, u32 polls)
{
    if (!polls || polls > 100000 || ((address != 0x1741 || value != 180) &&
        (address != 0x1745 || value != 0x80) &&
        (address != 0x1746 || value != 0x80))) { return Result::Invalid; }
    return transfer(bus, 3, address, true, value, polls);
}

Result inspectGraphicsRail(Bus &bus, RailSnapshot &snapshot, u32 polls)
{
    if (!polls || polls > 100000) { return Result::Invalid; }
    return inspect(bus, snapshot, polls);
}

Result enableGraphicsRail(Bus &bus, Rail &rail, bool gpuCollapsed, u32 polls)
{
    if (rail.ready || rail.pinned) { return Result::NotReady; }
    if (!gpuCollapsed || !polls || polls > 100000) { return Result::Unqualified; }
    Result result = inspect(bus, rail.initial, polls);
    if (result != Result::Ok) { return result; }
    rail.final = rail.initial;
    if (ready(rail.initial)) { rail.ready = true; rail.pinned = true; return Result::Ok; }
    if (rail.initial.range || rail.initial.microvolts > 1355000 ||
        (rail.initial.enable & 0x7f) || (rail.initial.mode & 0x3f)) { return Result::Unqualified; }
    rail.pinned = true;
    const u16 addresses[] = {0x1741, 0x1745, 0x1746};
    const u8 values[] = {180, 0x80, 0x80};
    for (u32 index = rail.initial.microvolts >= 980000 ? 1u : 0u; index < 3; ++index) {
        rail.changed = true;
        u8 value = values[index];
        result = transfer(bus, 3, addresses[index], true, value, polls);
        if (result != Result::Ok) { return result; }
    }
    while (polls) {
        result = inspect(bus, rail.final, polls);
        if (result != Result::Ok) { return result; }
        if (ready(rail.final)) { rail.ready = true; return Result::Ok; }
        if (polls) { --polls; bus.io.delayUs(bus.io.context, 1); }
    }
    return fail(bus, Result::TimedOut);
}

Result verifyGraphicsRail(Bus &bus, Rail &rail, u32 samples, u32 delayUs, u32 polls)
{
    if (bus.state != State::Ready || !rail.pinned || !rail.ready) { return Result::NotReady; }
    if (!samples || samples > 256 || delayUs > 1000 || !polls || polls > 100000 ||
        static_cast<u64>(samples) * delayUs > 250000 ||
        rail.stableSamples > 0xffffffffu - samples) { return Result::Invalid; }
    const RailSnapshot expected = rail.final;
    rail.ready = false;
    for (u32 index = 0; index < samples; ++index) {
        bus.io.delayUs(bus.io.context, delayUs);
        const Result result = inspect(bus, rail.final, polls);
        if (result != Result::Ok) { return result; }
        if (!ready(rail.final) || rail.final.range != expected.range ||
            rail.final.voltage != expected.voltage || rail.final.mode != expected.mode ||
            rail.final.enable != expected.enable) { return Result::Unqualified; }
        ++rail.stableSamples;
    }
    rail.ready = true;
    return Result::Ok;
}

bool graphicsReadCommand(u32 offset, u32 command)
{
    if (offset != kCommandOffset) { return false; }
    const u16 addresses[] = {0x1704, 0x1705, 0x1708, 0x1740, 0x1741, 0x1745, 0x1746};
    for (u16 address : addresses) {
        const u32 expected = (1u << 27) | (3u << 20) | (static_cast<u32>(address >> 8) << 12) |
            (static_cast<u32>(address & 255) << 4);
        if (command == expected) { return true; }
    }
    return false;
}

bool knownGraphicsPolicy(const RailSnapshot &snapshot)
{
    return snapshot.type == 0x1c && snapshot.subtype == 9 && snapshot.range == 0 &&
        (snapshot.mode == 0 || snapshot.mode == 0x40 || snapshot.mode == 0x80) &&
        (snapshot.enable == 0 || snapshot.enable == 0x80);
}

namespace {

Result observeWindow(Bus &bus, ObservedRail &rail, bool rpmManaged,
                     u32 samples, u32 attempts, u32 delayUs, u32 polls)
{
    if (bus.state != State::Ready || rail.pinned || rail.ready) { return Result::NotReady; }
    if (!samples || attempts < samples || attempts > 256 || !delayUs || delayUs > 1000 ||
        !polls || polls > 100000 || static_cast<u64>(attempts) * delayUs > 250000 ||
        rail.samples || rail.stableSamples) { return Result::Invalid; }
    rail.pinned = true;
    rail.rpmManaged = rpmManaged;
    for (u32 attempt = 0; attempt < attempts; ++attempt) {
        bus.io.delayUs(bus.io.context, delayUs);
        const Result result = observe(bus, rail, polls);
        if (result != Result::Ok) { return result; }
        if (!observedReady(rail.final, rail.rpmManaged)) {
            if (rail.stableSamples || rail.final.microvolts > lumia950::kMaximumGpuMicrovolts) {
                return Result::Unqualified;
            }
            continue;
        }
        ++rail.stableSamples;
        if (rail.stableSamples == samples) { rail.ready = true; return Result::Ok; }
    }
    return Result::TimedOut;
}

}

Result awaitGraphicsRail(Bus &bus, ObservedRail &rail, u32 samples, u32 attempts, u32 delayUs, u32 polls)
{
    return observeWindow(bus, rail, false, samples, attempts, delayUs, polls);
}

bool validRpmSupplyContract(const RpmSupplyContract &contract)
{
    return contract.clock.type == rpm::kGraphicsParent.type && contract.clock.id == rpm::kGraphicsParent.id &&
        contract.kilohertz == lumia950::kBootGpuHz / 1000 && contract.correlatedAck &&
        contract.exclusiveColdGpu && contract.oemPolicySelected;
}

Result awaitManagedGraphicsRail(Bus &bus, ObservedRail &rail, const RpmSupplyContract &contract,
                               u32 samples, u32 attempts, u32 delayUs, u32 polls)
{
    if (!validRpmSupplyContract(contract)) { return Result::Unqualified; }
    return observeWindow(bus, rail, true, samples, attempts, delayUs, polls);
}

Result recheckGraphicsRail(Bus &bus, ObservedRail &rail, u32 polls)
{
    if (bus.state != State::Ready || !rail.pinned || !rail.ready) { return Result::NotReady; }
    if (!polls || polls > 100000 || rail.samples == 0xffffffffu ||
        rail.stableSamples == 0xffffffffu) { return Result::Invalid; }
    rail.ready = false;
    const Result result = observe(bus, rail, polls);
    if (result != Result::Ok) { return result; }
    if (!observedReady(rail.final, rail.rpmManaged)) { return Result::Unqualified; }
    ++rail.stableSamples;
    rail.ready = true;
    return Result::Ok;
}

const char *resultName(Result result)
{
    switch (result) {
    case Result::Ok: return "OK";
    case Result::Invalid: return "INVALID";
    case Result::Unqualified: return "UNQUALIFIED";
    case Result::NotReady: return "NOT READY";
    case Result::IoError: return "IO ERROR";
    case Result::TimedOut: return "TIMED OUT";
    case Result::Denied: return "DENIED";
    case Result::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

}
}
