#include "SmdMemory.h"

namespace adreno {
namespace rpm {
namespace {

bool validRegion(const MemoryRegion &region)
{
    const u64 maximum = ~static_cast<u64>(0);
    return region.physical && region.address && region.bytes >= 4 &&
        !((region.physical | region.address | region.bytes) & 3) &&
        region.physical <= maximum - region.bytes && region.address <= maximum - region.bytes;
}

bool inMessageRam(const MemoryRegion &region)
{
    return validRegion(region) && region.physical >= kMessageRamPhysical &&
        region.physical < kMessageRamPhysical + kMessageRamBytes &&
        region.bytes <= kMessageRamPhysical + kMessageRamBytes - region.physical;
}

bool disjoint(u64 first, u32 firstBytes, u64 second, u32 secondBytes)
{
    return first + firstBytes <= second || second + secondBytes <= first;
}

bool validLayout(const MemoryLayout &layout)
{
    return inMessageRam(layout.control) && inMessageRam(layout.fifo) &&
        layout.control.bytes == 88 && layout.fifo.bytes >= 1024 &&
        layout.fifo.bytes <= 131072 && !(layout.fifo.bytes & (layout.fifo.bytes - 1)) &&
        validRegion(layout.doorbell) && layout.doorbell.physical == kRpmDoorbellPhysical &&
        layout.doorbell.bytes == 4 &&
        disjoint(layout.control.physical, layout.control.bytes, layout.fifo.physical, layout.fifo.bytes) &&
        disjoint(layout.control.address, layout.control.bytes, layout.fifo.address, layout.fifo.bytes) &&
        disjoint(layout.control.address, layout.control.bytes, layout.doorbell.address, layout.doorbell.bytes) &&
        disjoint(layout.fifo.address, layout.fifo.bytes, layout.doorbell.address, layout.doorbell.bytes);
}

bool translated(const MemoryAccess &access, const MemoryRegion &region)
{
    u32 offset = 0;
    for (;;) {
        const u64 address = region.address + offset;
        const u64 expected = region.physical + offset;
        for (u32 mode = 0; mode < 2; ++mode) {
            u64 par = 0;
            if (!access.translate(access.context, address, mode != 0, &par) || (par & 1) ||
                (par & 0x0000fffffffff000ull) != (expected & ~0xfffull) ||
                (address & 0xfff) != (expected & 0xfff)) { return false; }
            const u32 attributes = static_cast<u32>(par >> 56);
            if (attributes != 0 && attributes != 4) { return false; }
        }
        if (offset == region.bytes - 4) { return true; }
        const u64 nextPage = (address & ~0xfffull) + 4096;
        const u64 remaining = region.bytes - offset;
        offset += static_cast<u32>(nextPage - address < remaining ? nextPage - address : remaining - 4);
    }
}

bool mappingsCurrent(const MemoryBinding &binding)
{
    return binding.access.barrier(binding.access.context) &&
        translated(binding.access, binding.layout.control) &&
        translated(binding.access, binding.layout.fifo) &&
        translated(binding.access, binding.layout.doorbell);
}

bool accessible(const MemoryBinding &binding)
{
    return binding.state == MemoryState::Observed || binding.state == MemoryState::Owned;
}

bool fail(MemoryBinding &binding)
{
    binding.state = MemoryState::Poisoned;
    return false;
}

bool addressFor(const MemoryBinding &binding, SmdSpace space, u32 offset, u64 *address)
{
    if ((offset & 3) || !address) { return false; }
    const u32 half = binding.layout.fifo.bytes / 2;
    switch (space) {
    case SmdSpace::Control:
        if (offset >= 88) { return false; }
        *address = binding.layout.control.address + offset;
        return true;
    case SmdSpace::Transmit:
    case SmdSpace::Receive:
        if (offset >= half) { return false; }
        *address = binding.layout.fifo.address + (space == SmdSpace::Receive ? half : 0) + offset;
        return true;
    }
    return false;
}

bool memoryRead(void *context, SmdSpace space, u32 offset, u32 *value)
{
    MemoryBinding &binding = *static_cast<MemoryBinding *>(context);
    u64 address = 0;
    if (!accessible(binding) || !value || !addressFor(binding, space, offset, &address) ||
        (binding.state == MemoryState::Observed && space != SmdSpace::Control)) { return false; }
    return binding.access.read32(binding.access.context, address, value) || fail(binding);
}

bool ownedControlWord(u32 offset)
{
    switch (offset) {
    case 0: case 4: case 8: case 12: case 20: case 24: case 28: case 32: case 40:
    case 64: case 68: case 72: case 80:
        return true;
    default: return false;
    }
}

bool memoryWrite(void *context, SmdSpace space, u32 offset, u32 value)
{
    MemoryBinding &binding = *static_cast<MemoryBinding *>(context);
    u64 address = 0;
    if (binding.state != MemoryState::Owned || space == SmdSpace::Receive ||
        (space == SmdSpace::Control && !ownedControlWord(offset)) ||
        !addressFor(binding, space, offset, &address)) { return false; }
    return binding.access.write32(binding.access.context, address, value) || fail(binding);
}

bool memoryBarrier(void *context)
{
    MemoryBinding &binding = *static_cast<MemoryBinding *>(context);
    if (!accessible(binding)) { return false; }
    return binding.access.barrier(binding.access.context) || fail(binding);
}

bool memoryNotify(void *context)
{
    MemoryBinding &binding = *static_cast<MemoryBinding *>(context);
    if (binding.state != MemoryState::Owned) { return false; }
    return (binding.access.barrier(binding.access.context) &&
        binding.access.write32(binding.access.context, binding.layout.doorbell.address, 1) &&
        binding.access.barrier(binding.access.context)) || fail(binding);
}

void memoryDelay(void *context, u32 microseconds)
{
    MemoryBinding &binding = *static_cast<MemoryBinding *>(context);
    if (accessible(binding) && microseconds <= 1000) {
        binding.access.delayUs(binding.access.context, microseconds);
    }
}

}

Result observeMemory(MemoryBinding &binding, const MemoryLayout &layout,
                     const MemoryAccess &access)
{
    if (binding.state != MemoryState::Empty) { return Result::NotReady; }
    if (!validLayout(layout) || !access.translate || !access.read32 || !access.write32 ||
        !access.barrier || !access.delayUs) { return Result::Invalid; }
    if (!access.nonSecureEl1) { return Result::Unowned; }
    MemoryBinding candidate = {layout, access, MemoryState::Observed};
    if (!mappingsCurrent(candidate)) { return Result::NotReady; }
    binding = candidate;
    return Result::Ok;
}

Result snapshotChannel(MemoryBinding &binding, ChannelSnapshot &snapshot)
{
    snapshot = {};
    if (!accessible(binding)) { return Result::NotReady; }
    if (!mappingsCurrent(binding)) { fail(binding); return Result::NotReady; }
    ChannelSnapshot candidate = {};
    if (!memoryBarrier(&binding)) { return Result::IoError; }
    for (u32 index = 0; index < 22; ++index) {
        if (!memoryRead(&binding, SmdSpace::Control, index * 4, &candidate.words[index])) {
            return Result::IoError;
        }
    }
    if (!memoryBarrier(&binding)) { return Result::IoError; }
    snapshot = candidate;
    return Result::Ok;
}

bool idleAndEmpty(const ChannelSnapshot &snapshot, u32 fifoBytes)
{
    if (fifoBytes < 512 || fifoBytes > 65536 || (fifoBytes & (fifoBytes - 1))) { return false; }
    if (snapshot.words[0] != 0 || (snapshot.words[11] != 0 && snapshot.words[11] != 4)) { return false; }
    if (snapshot.words[1] != 0 || snapshot.words[12] > 1 ||
        (snapshot.words[11] == 0 && snapshot.words[12] != 0)) { return false; }
    for (u32 endpoint = 0; endpoint < 2; ++endpoint) {
        const u32 half = endpoint * 11;
        const u32 reader = snapshot.words[half + 9], writer = snapshot.words[half + 10];
        if (reader != writer ||
            reader >= fifoBytes || (reader & 3)) { return false; }
    }
    return true;
}

bool closedAndEmpty(const ChannelSnapshot &snapshot, u32 fifoBytes)
{
    return snapshot.words[11] == 0 && idleAndEmpty(snapshot, fifoBytes);
}

namespace {

Result stableIdle(MemoryBinding &binding)
{
    if (!mappingsCurrent(binding)) { fail(binding); return Result::NotReady; }
    ChannelSnapshot previous = {};
    for (u32 sample = 0; sample < 8; ++sample) {
        ChannelSnapshot current = {};
        if (snapshotChannel(binding, current) != Result::Ok) { return Result::IoError; }
        if (!idleAndEmpty(current, binding.layout.fifo.bytes / 2)) { return Result::NotReady; }
        if (sample) {
            for (u32 index = 0; index < 22; ++index) {
                if (previous.words[index] != current.words[index]) { return Result::NotReady; }
            }
        }
        previous = current;
        if (sample != 7) { binding.access.delayUs(binding.access.context, 1000); }
    }
    return Result::Ok;
}

}

Result claimMemory(MemoryBinding &binding, bool firmwareExited, bool exclusiveOwner)
{
    if (binding.state != MemoryState::Observed) { return Result::NotReady; }
    if (!firmwareExited || !exclusiveOwner) { return Result::Unowned; }
    const Result qualified = stableIdle(binding);
    if (qualified == Result::Ok) { binding.state = MemoryState::Owned; }
    return qualified;
}

Result retireMemory(MemoryBinding &binding, const SmdPipe &pipe)
{
    if (binding.state != MemoryState::Owned || pipe.state != PipeState::Closed) { return Result::NotReady; }
    const Result qualified = stableIdle(binding);
    if (qualified != Result::Ok) { fail(binding); return qualified; }
    binding.state = MemoryState::Retired;
    return Result::Ok;
}

SmdIo memoryIo(MemoryBinding &binding)
{
    return {&binding, memoryRead, memoryWrite, memoryBarrier, memoryNotify, memoryDelay};
}

}
}
