#include "SpmiCore.h"
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
        device.registers[byte] = static_cast<u8>(device.writeData);
        if (device.registers[0x46] == 0x80 && !device.neverReady) { device.registers[8] = 0x80; }
        return true;
    }
    static void barrier(void *) {}
    static void delay(void *context, u32 duration)
    {
        static_cast<Device *>(context)->delays += duration;
    }
    spmi::Io io() { return {this, read, write, barrier, delay}; }
};
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
    printf("Native SPMI/graphics supply: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
