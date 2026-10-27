#include "SpmiCore.h"
#include "PinnedGraphics.h"
#include <stdio.h>

using namespace adreno;

namespace {
unsigned checks;
unsigned failures;
void expect(bool passed, const char *name)
{
    ++checks;
    if (!passed) { ++failures; printf("FAIL %s\n", name); }
}

struct Device {
    u8 registers[256] = {};
    u32 version = 0x10000000;
    u32 configuration = 0;
    u32 completion = 1;
    u32 data = 0;
    u32 writeData = 0;
    u32 commands = 0;
    u32 writes = 0;
    u32 statusReads = 0;
    u32 delays = 0;
    bool wrongSlave = false;
    bool neverReady = false;
    bool requireClockFirst = false;
    bool clockAcknowledged = false;
    bool wrongOrder = false;
    bool wrongResource = false;
    bool missingAck = false;
    bool lowerOnClockAck = true;
    u32 sends = 0;
    u32 pendingMessage = 0;
    u32 lowerAfterUs = 0;
    Device()
    {
        registers[4] = 0x1c; registers[5] = 9;
        registers[0x40] = 0; registers[0x41] = 164;
    }
    static bool read(void *context, u32 offset, u32 *value)
    {
        Device &device = *static_cast<Device *>(context);
        switch (offset) {
        case spmi::kVersionOffset: *value = device.version; return true;
        case spmi::kConfigurationOffset: *value = device.configuration; return true;
        case spmi::kStatusOffset: ++device.statusReads; *value = device.completion; return true;
        case spmi::kReadDataOffset: *value = device.data; return true;
        default: return false;
        }
    }
    static bool write(void *context, u32 offset, u32 value)
    {
        Device &device = *static_cast<Device *>(context);
        if (offset == spmi::kWriteDataOffset) { device.writeData = value; return true; }
        if (offset != spmi::kCommandOffset) { return false; }
        ++device.commands;
        const u32 opcode = value >> 27;
        const u32 slave = (value >> 20) & 15;
        const u32 address = (value >> 12) & 255;
        const u32 byte = (value >> 4) & 255;
        if (slave != 3 || address != 0x17 || (value & 15)) { device.wrongSlave = true; return false; }
        if (opcode == 1) { device.data = device.registers[byte]; return true; }
        if (opcode != 0 || (byte != 0x41 && byte != 0x45 && byte != 0x46)) { return false; }
        ++device.writes;
        if (device.requireClockFirst && !device.clockAcknowledged) { device.wrongOrder = true; }
        device.registers[byte] = static_cast<u8>(device.writeData);
        if (device.registers[0x46] == 0x80 && !device.neverReady) { device.registers[8] = 0x80; }
        return true;
    }
    static void barrier(void *) {}
    static void delay(void *context, u32 duration)
    {
        auto &device = *static_cast<Device *>(context);
        device.delays += duration;
        if (device.lowerAfterUs && device.delays >= device.lowerAfterUs) {
            device.registers[0x41] = 124;
        }
    }
    spmi::Io io() { return {this, read, write, barrier, delay}; }
    static u32 word(const u8 *bytes)
    {
        return bytes[0] | (static_cast<u32>(bytes[1]) << 8) |
            (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
    }
    static rpm::Transfer send(void *context, const u8 *bytes, u32 size)
    {
        auto &device = *static_cast<Device *>(context);
        ++device.sends;
        device.wrongOrder |= device.writes != 0;
        device.wrongResource |= size != 40 || word(bytes + 12) != 0 ||
            word(bytes + 16) != rpm::kGraphicsParent.type || word(bytes + 20) != rpm::kGraphicsParent.id ||
            word(bytes + 28) != rpm::kRateKey || word(bytes + 36) != pinned::kGraphicsKHz;
        device.pendingMessage = word(bytes + 8);
        return rpm::Transfer::Ok;
    }
    static rpm::Transfer receive(void *context, u8 *bytes, u32 capacity, u32 *size)
    {
        auto &device = *static_cast<Device *>(context);
        if (device.missingAck || !device.pendingMessage) { return rpm::Transfer::WouldBlock; }
        if (capacity < 20) { return rpm::Transfer::IoError; }
        const u32 words[] = {rpm::kRequestService, 12, rpm::kMessageIdKey, 4, device.pendingMessage};
        for (u32 index = 0; index < 5; ++index) {
            for (u32 byte = 0; byte < 4; ++byte) {
                bytes[index * 4 + byte] = static_cast<u8>(words[index] >> (byte * 8));
            }
        }
        *size = 20;
        device.pendingMessage = 0;
        device.clockAcknowledged = true;
        if (device.lowerOnClockAck) { device.registers[0x41] = 124; }
        return rpm::Transfer::Ok;
    }
    rpm::Transport transport() { return {this, send, receive, delay}; }
};

void testClockSupplyHandoff()
{
    Device device;
    device.version = 0x20000002;
    device.requireClockFirst = true;
    spmi::Bus bus = {};
    rpm::Session session = {};
    pinned::Handoff handoff = {};
    expect(spmi::bind(bus, device.io(), true, true) == spmi::Result::Ok &&
        rpm::bindSession(session, device.transport(), 0x41810000, true) == rpm::Result::Ok,
        "bind measured handset SPMI version and native RPM session");
    expect(pinned::prepare(handoff, bus, session, false) == pinned::Result::Unqualified &&
        !device.commands && !device.sends, "no clock or supply request without exclusive cold GPU ownership");
    expect(pinned::prepare(handoff, bus, session, true) == pinned::Result::Ok &&
        handoff.phase == pinned::Phase::Ready && handoff.rail.ready,
        "clock ACK changing the rail to 700mV precedes a single fixed supply establishment");
    expect(handoff.inherited.microvolts == 900000 && handoff.rail.initial.microvolts == 700000 &&
        handoff.rail.final.microvolts == 980000, "retain inherited post-clock and final real supply snapshots");
    expect(device.sends == 1 && device.writes == 3 && !device.wrongOrder && !device.wrongResource,
        "only the graphics clock resource precedes allowed native graphics supply writes");
    expect(handoff.rail.stableSamples == pinned::kSupplySamples &&
        device.delays >= pinned::kSupplySamples * pinned::kSupplySampleDelayUs,
        "fixed rail must survive the complete bounded 16ms stability window");
    const u32 commands = device.commands;
    expect(pinned::prepare(handoff, bus, session, true) == pinned::Result::NotReady &&
        device.commands == commands && device.sends == 1 && device.writes == 3,
        "pinned prerequisites cannot be replayed or change clocks after supply establishment");
    expect(spmi::verifyGraphicsRail(bus, handoff.rail, 1, 0, 100) == spmi::Result::Ok &&
        handoff.rail.stableSamples == pinned::kSupplySamples + 1,
        "later domain transitions can revalidate the same rail without writing it");
    device.registers[0x41] = 124;
    expect(spmi::verifyGraphicsRail(bus, handoff.rail, 1, 0, 100) == spmi::Result::Unqualified &&
        !handoff.rail.ready && handoff.rail.pinned && device.writes == 3,
        "a later voltage drop invalidates access rather than fighting the other writer");
    for (u32 mutation = 0; mutation < 4; ++mutation) {
        Device failed;
        failed.requireClockFirst = true;
        if (mutation == 0) { failed.missingAck = true; }
        if (mutation == 1) { failed.lowerAfterUs = 3000; }
        if (mutation == 2) { failed.registers[0x45] = 1; }
        if (mutation == 3) { failed.neverReady = true; }
        spmi::Bus failedBus = {};
        rpm::Session failedSession = {};
        pinned::Handoff failedHandoff = {};
        spmi::bind(failedBus, failed.io(), true, true);
        rpm::bindSession(failedSession, failed.transport(), 0x41810000, true);
        expect(pinned::prepare(failedHandoff, failedBus, failedSession, true) != pinned::Result::Ok &&
            !failedHandoff.rail.ready && !failed.wrongOrder && failed.sends <= 1 && failed.writes <= 3,
            "missing ACK competing writer unknown pin policy and missing readiness never qualify or retry");
        if (mutation == 0 || mutation == 2) {
            expect(!failed.writes, "clock failure and unknown supply policy never write PMIC registers");
        }
        if (mutation == 1) {
            expect(failedHandoff.phase == pinned::Phase::Settle && failedHandoff.rail.pinned &&
                failedHandoff.rail.stableSamples < pinned::kSupplySamples && failedHandoff.rail.final.microvolts == 700000,
                "a sustained competing owner stops with the actual drift snapshot and pinned state");
        }
    }
    Device inherited;
    inherited.lowerOnClockAck = false;
    inherited.registers[0x41] = 204; inherited.registers[0x45] = 0x80;
    inherited.registers[0x46] = 0x80; inherited.registers[8] = 0x80;
    spmi::Bus inheritedBus = {};
    rpm::Session inheritedSession = {};
    pinned::Handoff inheritedHandoff = {};
    spmi::bind(inheritedBus, inherited.io(), true, true);
    rpm::bindSession(inheritedSession, inherited.transport(), 0x41810000, true);
    expect(pinned::prepare(inheritedHandoff, inheritedBus, inheritedSession, true) == pinned::Result::Ok &&
        inheritedHandoff.rail.final.microvolts == 1100000 && !inherited.writes,
        "a stable adequate inherited voltage is not lowered even after clock ACK");
    expect(spmi::verifyGraphicsRail(inheritedBus, inheritedHandoff.rail, 0, 0, 100) == spmi::Result::Invalid,
        "an empty stability window cannot establish readiness");
    spmi::Bus unbound = {};
    expect(spmi::verifyGraphicsRail(unbound, inheritedHandoff.rail, 1, 0, 100) == spmi::Result::NotReady,
        "a rail snapshot cannot substitute for a bound native bus");
    expect(spmi::verifyGraphicsRail(inheritedBus, inheritedHandoff.rail, 2, 250, 8) == spmi::Result::TimedOut &&
        !inheritedHandoff.rail.ready && inheritedHandoff.rail.pinned && !inherited.writes,
        "all stability snapshots share one finite SPMI transaction budget");
}
}

int main()
{
    using namespace spmi;
    Device model;
    Bus bus = {};
    expect(bind(bus, model.io(), false, true) == Result::Unqualified && !model.commands,
        "no native SPMI ownership before firmware exit");
    expect(bind(bus, model.io(), true, true) == Result::Ok, "native V1 channel bound");
    Rail rail = {};
    expect(enableGraphicsRail(bus, rail, false, 100) == Result::Unqualified && !model.commands,
        "no rail mutation while GPU may be active");
    expect(enableGraphicsRail(bus, rail, true, 100) == Result::Ok && rail.ready && rail.pinned,
        "cold PMI8994 graphics rail enabled");
    expect(rail.initial.microvolts == 900000 && rail.final.microvolts == 980000,
        "FTSMPS2p5 range and voltage decoded");
    expect(model.writes == 3 && model.registers[0x41] == 180 && !model.wrongSlave,
        "only graphics peripheral voltage mode enable written");
    expect(enableGraphicsRail(bus, rail, true, 100) == Result::NotReady, "no repeat activation");
    expect(writeGraphicsByte(bus, 0x1741, 200, 100) == Result::Invalid && model.writes == 3,
        "voltage outside explicit board setting rejected");
    expect(writeGraphicsByte(bus, 0x1740, 1, 100) == Result::Invalid, "range changes forbidden");
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        Device bad;
        Bus refused = {};
        Rail refusedRail = {};
        switch (mutation) {
        case 0: bad.version = 0x20010000; break;
        case 1: bad.configuration = 1; break;
        case 2: bad.registers[4] = 3; break;
        case 3: bad.registers[5] = 8; break;
        case 4: bad.registers[0x40] = 1; break;
        case 5: bad.registers[0x45] = 1; break;
        case 6: bad.registers[0x46] = 1; break;
        }
        const Result bound = bind(refused, bad.io(), true, true);
        const Result result = bound == Result::Ok ? enableGraphicsRail(refused, refusedRail, true, 100) : bound;
        expect(result == Result::Unqualified && !bad.writes, "unknown controller regulator or pin policy refused before PMIC writes");
    }
    const u32 completions[] = {0u, 2u, 4u, 8u, 0xffffffffu};
    for (u32 completion : completions) {
        Device failed;
        failed.completion = completion;
        Bus poisoned = {};
        bind(poisoned, failed.io(), true, true);
        Rail held = {};
        const Result result = enableGraphicsRail(poisoned, held, true, 31);
        expect(result != Result::Ok && poisoned.state == State::Poisoned && failed.statusReads <= 31,
            "transport timeout error denied and unknown status poison within one shared budget");
        const u32 commands = failed.commands;
        u8 value = 0;
        expect(readByte(poisoned, 3, 0x1704, value, 31) == Result::NotReady && commands == failed.commands,
            "uncertain command not replayed");
    }
    Device waiting;
    waiting.neverReady = true;
    Bus waitingBus = {};
    bind(waitingBus, waiting.io(), true, true);
    Rail waitingRail = {};
    expect(enableGraphicsRail(waitingBus, waitingRail, true, 31) == Result::TimedOut &&
        waiting.statusReads <= 31 && waitingRail.pinned, "readiness retries share total budget and retain supply");
    Device inherited;
    inherited.registers[0x41] = 204; inherited.registers[0x45] = 0x80;
    inherited.registers[0x46] = 0x80; inherited.registers[8] = 0x80;
    Bus inheritedBus = {};
    Rail inheritedRail = {};
    bind(inheritedBus, inherited.io(), true, true);
    expect(enableGraphicsRail(inheritedBus, inheritedRail, true, 31) == Result::Ok && !inherited.writes &&
        inheritedRail.final.microvolts == 1100000, "existing adequate graphics voltage is retained without lowering");
    testClockSupplyHandoff();
    printf("Native SPMI/graphics supply: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
