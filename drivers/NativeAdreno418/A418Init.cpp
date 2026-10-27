#include "A418Init.h"
#include "BoardConstants.h"

namespace adreno {
namespace {

constexpr u32 kPowerControl = 0x98;
constexpr u32 kPowerStatus = 0x1b0;
constexpr u32 kSpTpOn = 1u << 20;
struct Setting { u32 index; u32 value; };
constexpr Setting kClockSettings[] = {
    {0x4, 0x2222202u},
    {0x5, 0x2222202u},
    {0x6, 0x2222202u},
    {0x7, 0x2222202u},
    {0x8, 0x2222u},
    {0x9, 0x2222u},
    {0xa, 0x2222u},
    {0xb, 0x2222u},
    {0xc, 0xe739ce7u},
    {0xd, 0xe739ce7u},
    {0xe, 0xe739ce7u},
    {0xf, 0xe739ce7u},
    {0x10, 0x111111u},
    {0x11, 0x111111u},
    {0x12, 0x111111u},
    {0x13, 0x111111u},
    {0x68, 0x22222222u},
    {0x69, 0x22222222u},
    {0x6a, 0x22222222u},
    {0x6b, 0x22222222u},
    {0x6c, 0x222222u},
    {0x6d, 0x222222u},
    {0x6e, 0x222222u},
    {0x6f, 0x222222u},
    {0x70, 0x104u},
    {0x71, 0x104u},
    {0x72, 0x104u},
    {0x73, 0x104u},
    {0x74, 0x81u},
    {0x75, 0x81u},
    {0x76, 0x81u},
    {0x77, 0x81u},
    {0x14, 0x22222222u},
    {0x15, 0x2222222u},
    {0x16, 0x0u},
    {0x17, 0x0u},
    {0x18, 0x4444u},
    {0x19, 0x1112u},
    {0x78, 0x22222222u},
    {0x79, 0x22222222u},
    {0x7a, 0x22222222u},
    {0x7b, 0x22222222u},
    {0x7c, 0x22020u},
    {0x7d, 0x22020u},
    {0x7e, 0x22020u},
    {0x7f, 0x22020u},
    {0x82, 0x922u},
    {0x83, 0x922u},
    {0x84, 0x922u},
    {0x85, 0x922u},
    {0x86, 0x0u},
    {0x87, 0x0u},
    {0x88, 0x0u},
    {0x89, 0x0u},
    {0x8e, 0x1u},
    {0x8f, 0x1u},
    {0x90, 0x1u},
    {0x91, 0x1u},
    {0x1a, 0x2222222u},
    {0x1c, 0x4100104u},
    {0x1b, 0x22222u},
    {0x81, 0x22u},
    {0x80, 0x10fu},
    {0x8d, 0x22u},
    {0x1d, 0x222222u},
    {0x1e, 0x4104u},
    {0x1f, 0x222u},
    {0x8b, 0x0u},
    {0x8c, 0x220000u},
    {0x20, 0xaaaaaaaau}
};
constexpr Setting kCommandSettings[] = {
    {0x302a, 1}, {0x302c, 0x18181818}, {0x302d, 0x18},
    {0x3030, 0x18181818}, {0x3031, 0x18}, {0x3049, 3},
    {0x17a, 0xffffffffu}, {0x21, 0x10}, {0x2b, 0x10},
    {0x23, 1}, {0x24, 0xa6ffffffu}, {0x3e, 0x30},
    {0x2f, 0x4000ffff}, {0x170, 1}, {0xe8c, 7},
    {0xe83, 0xffff0000u}, {0xe84, 0xffff0000u},
    {0x22e, (1u << 25) | (1u << 14)}, {0x99, 0x441}, {0x9a, 0x441},
    {0x37, 0}
};
struct Protection { u32 first; u32 exponent; };
constexpr Protection kProtections[] = {
    {0x4, 2}, {0x8, 3}, {0x10, 4}, {0x20, 5}, {0x40, 6}, {0x80, 4},
    {0x200, 7}, {0x580, 4}, {0xcc0, 0}, {0xe00, 0}, {0xe60, 1},
    {0x2c00, 10}, {0x3300, 8}, {0x3400, 10}, {0x4000, 14}
};

HardwareResult fail(CommandHardware &hardware, HardwareResult result)
{
    hardware.state = HardwareState::Poisoned;
    return result;
}

bool read(CommandHardware &hardware, u32 index, u32 &value)
{
    hardware.lastIndex = index;
    if (!hardware.io.read(hardware.io.context, RegisterSpace::Gpu, index * 4, &value) ||
        value == 0xffffffffu) { return false; }
    hardware.lastValue = value;
    return true;
}

bool write(CommandHardware &hardware, u32 index, u32 value)
{
    hardware.lastIndex = index;
    hardware.lastValue = value;
    if (!hardware.io.write(hardware.io.context, RegisterSpace::Gpu, index * 4, value)) {
        return false;
    }
    hardware.io.barrier(hardware.io.context);
    return true;
}

HardwareResult waitSpTp(CommandHardware &hardware, bool enabled, u32 pollBudget)
{
    for (u32 attempt = 0; attempt < pollBudget; ++attempt) {
        u32 status = 0;
        if (!read(hardware, kPowerStatus, status)) { return HardwareResult::IoError; }
        ++hardware.polls;
        if (((status & kSpTpOn) != 0) == enabled) { return HardwareResult::Ok; }
        if (attempt + 1 < pollBudget) { hardware.io.delayUs(hardware.io.context, 5); }
    }
    return HardwareResult::TimedOut;
}

u32 protectionIndex(u32 slot)
{
    return 0x240 + slot;
}

u32 protectionValue(const Protection &protection)
{
    return 0x60000000u | (protection.exponent << 24) | (protection.first << 2);
}

}

bool validHardwareContract(const HardwareContract &contract)
{
    return contract.chipId == lumia950::kChipId && contract.identityQualified &&
        contract.exclusiveOwnership;
}

HardwareResult initializeCommandHardware(CommandHardware &hardware, const RegisterIo &io,
    const HardwareContract &contract, u32 pollBudget)
{
    if (hardware.state != HardwareState::Empty) { return HardwareResult::NotReady; }
    if (!validHardwareContract(contract)) { return HardwareResult::Unqualified; }
    if (!io.read || !io.write || !io.barrier || !io.delayUs ||
        !pollBudget || pollBudget > 100000) { return HardwareResult::Invalid; }
    GpuSnapshot snapshot = {};
    if (readSnapshot(io, &snapshot) != ProbeResult::Ready ||
        (snapshot.status & kGpuBusy) || (snapshot.interruptStatus & kGpuFaultMask) ||
        snapshot.readPointer || snapshot.writePointer) { return HardwareResult::NotReady; }
    hardware.io = io;
    hardware.state = HardwareState::Programming;
    hardware.version = snapshot.version;
    if (!write(hardware, lumia950::kRegCpMeCntl, kMeHalt)) {
        return fail(hardware, HardwareResult::IoError);
    }
    u32 control = 0;
    if (!read(hardware, lumia950::kRegCpMeCntl, control) || (control & kMeHalt) == 0) {
        return fail(hardware, HardwareResult::Mismatch);
    }
    if (!write(hardware, kPowerControl, 0x778000)) {
        return fail(hardware, HardwareResult::IoError);
    }
    const HardwareResult powered = waitSpTp(hardware, true, pollBudget);
    if (powered != HardwareResult::Ok) { return fail(hardware, powered); }
    for (const Setting &setting : kCommandSettings) {
        if (!write(hardware, setting.index, setting.value)) { return fail(hardware, HardwareResult::IoError); }
    }
    for (const Setting &setting : kClockSettings) {
        if (!write(hardware, setting.index, setting.value)) { return fail(hardware, HardwareResult::IoError); }
    }
    for (u32 slot = 0; slot < sizeof(kProtections) / sizeof(kProtections[0]); ++slot) {
        const u32 index = protectionIndex(slot);
        const u32 expected = protectionValue(kProtections[slot]);
        u32 observed = 0;
        if (!write(hardware, index, expected) || !read(hardware, index, observed)) {
            return fail(hardware, HardwareResult::IoError);
        }
        if (observed != expected) { return fail(hardware, HardwareResult::Mismatch); }
    }
    if (!write(hardware, 0x250, 7) || !read(hardware, 0x250, control) || control != 7) {
        return fail(hardware, HardwareResult::Mismatch);
    }
    if (readSnapshot(io, &snapshot) != ProbeResult::Ready ||
        (snapshot.status & kGpuBusy) || (snapshot.interruptStatus & kGpuFaultMask) ||
        (snapshot.meControl & kMeHalt) == 0) { return fail(hardware, HardwareResult::NotReady); }
    hardware.state = HardwareState::Ready;
    return HardwareResult::Ok;
}

HardwareResult stopCommandHardware(CommandHardware &hardware, bool translationsDisabled,
    u32 pollBudget)
{
    if (hardware.state != HardwareState::Ready || !translationsDisabled) { return HardwareResult::NotReady; }
    if (!pollBudget || pollBudget > 100000) { return HardwareResult::Invalid; }
    GpuSnapshot snapshot = {};
    if (readSnapshot(hardware.io, &snapshot) != ProbeResult::Ready ||
        (snapshot.status & kGpuBusy) || (snapshot.meControl & kMeHalt) == 0 ||
        (snapshot.interruptStatus & kGpuFaultMask)) { return HardwareResult::NotReady; }
    hardware.state = HardwareState::Stopping;
    if (!write(hardware, kPowerControl, 0x778001)) {
        return fail(hardware, HardwareResult::IoError);
    }
    const HardwareResult powered = waitSpTp(hardware, false, pollBudget);
    if (powered != HardwareResult::Ok) { return fail(hardware, powered); }
    hardware.state = HardwareState::Stopped;
    return HardwareResult::Ok;
}

}
