#include "SmdMemory.h"
#include <array>
#include <cstdio>
#include <vector>

using namespace adreno;
using namespace adreno::rpm;

namespace {
unsigned checks;
unsigned failures;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", message); }
}

struct Device {
    MemoryLayout layout = {{kMessageRamPhysical, 0x100000, 88},
                           {kMessageRamPhysical + 0x200, 0x100200, 2048},
                           {kRpmDoorbellPhysical, 0x200008, 4}};
    std::array<u32, 22> control = {};
    std::array<u32, 512> fifo = {};
    struct Operation { char kind; u64 address; u32 value; };
    std::vector<Operation> operations;
    u32 attributes = 0;
    bool clearNsBit = false, unknownWorld = false, writeFault = false, fault = false, wrongPhysical = false;
    bool failTranslate = false, failRead = false, failWrite = false, failBarrier = false;
    bool peer = true, dirtyClose = false, remoteReset = false, delayedClose = false;
    bool closingIdle = false, delayedOpen = false;
    u32 openingReply = 2, openedReply = 2, openedAfterDelay = 0;
    u64 faultAddress = 0;
    unsigned reads = 0, writes = 0, translations = 0, delays = 0, notifications = 0;
    u32 mutateAfterDelay = 0;

    static bool translate(void *context, u64 address, bool writing, u64 *par)
    {
        auto &device = *static_cast<Device *>(context);
        ++device.translations;
        device.operations.push_back({writing ? 'W' : 'R', address, 0});
        if (device.failTranslate) { return false; }
        const MemoryRegion regions[] = {device.layout.control, device.layout.fifo, device.layout.doorbell};
        for (const MemoryRegion &region : regions) {
            if (address < region.address || address >= region.address + region.bytes) { continue; }
            const u64 physical = region.physical + address - region.address;
            *par = (physical & ~0xfffull) | (static_cast<u64>(device.attributes) << 56) |
                (device.clearNsBit ? 0 : 0x200u) | (device.wrongPhysical ? 0x100000000ull : 0);
            if (device.fault || (writing && device.writeFault) || address == device.faultAddress) { *par |= 1; }
            return true;
        }
        return false;
    }
    u32 *wordAt(u64 address)
    {
        if (address >= layout.control.address && address < layout.control.address + 88) {
            return &control[(address - layout.control.address) / 4];
        }
        if (address >= layout.fifo.address && address < layout.fifo.address + layout.fifo.bytes) {
            return &fifo[(address - layout.fifo.address) / 4];
        }
        return nullptr;
    }
    static bool read(void *context, u64 address, u32 *value)
    {
        auto &device = *static_cast<Device *>(context);
        ++device.reads;
        device.operations.push_back({'L', address, 0});
        u32 *source = device.wordAt(address);
        if (device.failRead || !source || !value || (address & 3)) { return false; }
        *value = *source;
        return true;
    }
    static bool write(void *context, u64 address, u32 value)
    {
        auto &device = *static_cast<Device *>(context);
        ++device.writes;
        device.operations.push_back({'S', address, value});
        if (device.failWrite || (address & 3)) { return false; }
        if (address == device.layout.doorbell.address) {
            expect(value == 1, "native notification writes only BIT0, never a read-modify-write");
            ++device.notifications;
            if (!device.peer) { return true; }
            device.control[18] = 1;
            if (device.control[0] == 1) {
                device.control[11] = device.openingReply;
                device.control[12] = device.openingReply == 2 ? 1 : 0;
                device.control[9] = device.control[21] = 0;
            } else if (device.control[0] == 2) {
                device.control[11] = device.openedReply;
                device.control[12] = device.openedReply == 2 && !device.delayedOpen ? 1 : 0;
                if (device.delayedOpen) { device.openedAfterDelay = device.delays + 2; }
            } else if (device.control[0] == 4) {
                device.control[11] = device.remoteReset ? 5 : device.delayedClose || device.closingIdle ? 4 : 0;
                device.control[12] = 0;
            } else if (device.control[0] == 0) {
                device.control[11] = device.closingIdle ? 4 : 0;
                device.control[12] = 0;
                if (device.dirtyClose) { device.control[21] = 4; }
            }
            return true;
        }
        u32 *destination = device.wordAt(address);
        if (!destination) { return false; }
        *destination = value;
        return true;
    }
    static bool barrier(void *context)
    {
        auto &device = *static_cast<Device *>(context);
        device.operations.push_back({'B', 0, 0});
        return !device.failBarrier;
    }
    static void delay(void *context, u32 microseconds)
    {
        auto &device = *static_cast<Device *>(context);
        ++device.delays;
        if (device.openedAfterDelay && device.delays == device.openedAfterDelay) {
            device.control[11] = 2;
            device.control[12] = 1;
            device.control[18] = 1;
        }
        if (device.mutateAfterDelay && device.delays == device.mutateAfterDelay) { ++device.control[5]; }
        expect(microseconds <= 1000, "native delays respect the qualified transport bound");
    }
    MemoryAccess access() { return {this, translate, read, write, barrier, delay, !unknownWorld}; }
};

void testMappings()
{
    Device device;
    MemoryBinding binding = {};
    expect(observeMemory(binding, device.layout, device.access()) == Result::Ok &&
           binding.state == MemoryState::Observed && device.reads == 0 && device.writes == 0,
           "binding qualifies read/write translations without touching shared memory or IPC");
    expect(device.translations == 10, "first and last control/FIFO words and doorbell are permission checked");
    expect(observeMemory(binding, device.layout, device.access()) == Result::NotReady,
           "live mapping binding cannot be replaced");
    const SmdIo io = memoryIo(binding);
    u32 value = 0;
    expect(!io.write32(io.context, SmdSpace::Control, 0, 1) && !io.notify(io.context) && device.writes == 0,
           "observed pre-firmware-exit mapping cannot write or notify");
    expect(!io.read32(io.context, SmdSpace::Transmit, 0, &value) && device.reads == 0,
           "read-only observation is limited to the control block");
    ChannelSnapshot snapshot = {};
    expect(snapshotChannel(binding, snapshot) == Result::Ok && device.reads == 22 &&
           closedAndEmpty(snapshot, 1024), "bounded word reads snapshot exactly two 44-byte halves");
    expect(claimMemory(binding, false, true) == Result::Unowned &&
           claimMemory(binding, true, false) == Result::Unowned && device.writes == 0,
           "neither an open-looking channel nor an ownership assertion substitutes for firmware exit");
    for (u32 attributes : {0u, 4u, 8u, 12u, 0x44u, 0xffu}) {
        Device candidate;
        candidate.attributes = attributes;
        MemoryBinding mapped = {};
        expect((observeMemory(mapped, candidate.layout, candidate.access()) == Result::Ok) ==
               (attributes == 0 || attributes == 4),
               "only non-gathering Device attributes qualify this no-cache-maintenance implementation");
        expect(candidate.reads == 0 && candidate.writes == 0, "attribute refusal has no shared-memory side effects");
    }
    for (unsigned failure = 0; failure < 6; ++failure) {
        Device invalid;
        invalid.unknownWorld = failure == 0;
        invalid.writeFault = failure == 1;
        invalid.fault = failure == 2;
        invalid.wrongPhysical = failure == 3;
        invalid.failTranslate = failure == 4;
        invalid.failBarrier = failure == 5;
        MemoryBinding rejected = {};
        expect(observeMemory(rejected, invalid.layout, invalid.access()) ==
               (failure == 0 ? Result::Unowned : Result::NotReady) &&
               rejected.state == MemoryState::Empty && invalid.reads == 0 && invalid.writes == 0,
               "unknown execution world, inaccessible, aliased or unqualified mapping refuses before dereference");
    }
    Device undefinedNs;
    undefinedNs.clearNsBit = true;
    MemoryBinding nonSecure = {};
    expect(observeMemory(nonSecure, undefinedNs.layout, undefinedNs.access()) == Result::Ok,
           "PAR.NS is architecturally UNKNOWN in Non-secure state and cannot be used as a security gate");
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
        Device invalid;
        MemoryLayout layout = invalid.layout;
        switch (mutation) {
        case 0: layout.control.physical = 0x06a00000; break;
        case 1: layout.control.bytes = 44; break;
        case 2: layout.control.address = 0; break;
        case 3: layout.control.address |= 1; break;
        case 4: layout.control.address = ~static_cast<u64>(0) - 3; break;
        case 5: layout.fifo.physical = layout.control.physical; break;
        case 6: layout.fifo.address = layout.control.address; break;
        case 7: layout.fifo.bytes = 2047; break;
        case 8: layout.fifo.physical = kMessageRamPhysical + kMessageRamBytes - 4; break;
        case 9: layout.doorbell.physical += 4; break;
        case 10: layout.doorbell.bytes = 8; break;
        case 11: layout.doorbell.address = layout.fifo.address; break;
        }
        MemoryBinding rejected = {};
        expect(observeMemory(rejected, layout, invalid.access()) == Result::Invalid &&
               invalid.translations == 0 && invalid.reads == 0 && invalid.writes == 0,
               "malformed or unsupported physical/virtual layout is refused before translation");
    }
    Device crossed;
    crossed.layout.fifo.physical = kMessageRamPhysical + 0xff0;
    crossed.layout.fifo.address = 0x100ff0;
    crossed.faultAddress = 0x101000;
    MemoryBinding rejected = {};
    expect(observeMemory(rejected, crossed.layout, crossed.access()) == Result::NotReady,
           "every crossed 4 KiB translation boundary is validated, not just endpoints");
}

void testOwnership()
{
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        Device device;
        switch (mutation) {
        case 0: device.control[0] = 2; break;
        case 1: device.control[11] = 3; break;
        case 2: device.control[1] = 1; break;
        case 3: device.control[12] = 1; break;
        case 4: device.control[10] = 4; break;
        case 5: device.control[21] = 4; break;
        case 6: device.control[9] = device.control[10] = 1024; break;
        case 7: device.control[20] = device.control[21] = 1; break;
        }
        MemoryBinding binding = {};
        observeMemory(binding, device.layout, device.access());
        expect(claimMemory(binding, true, true) == Result::NotReady && device.writes == 0 &&
               binding.state == MemoryState::Observed, "non-quiescent state is never reset or stolen");
    }
    Device moving;
    moving.mutateAfterDelay = 2;
    MemoryBinding unstable = {};
    observeMemory(unstable, moving.layout, moving.access());
    expect(claimMemory(unstable, true, true) == Result::NotReady && moving.writes == 0,
           "changing remote metadata prevents claiming an apparently empty channel");
    Device changed;
    MemoryBinding remapped = {};
    observeMemory(remapped, changed.layout, changed.access());
    changed.attributes = 0xff;
    expect(claimMemory(remapped, true, true) == Result::NotReady &&
           remapped.state == MemoryState::Poisoned && changed.reads == 0 && changed.writes == 0,
           "handoff revalidates translations instead of trusting a pre-exit cacheability observation");
    Device changedSnapshot;
    MemoryBinding snapshotMapping = {};
    observeMemory(snapshotMapping, changedSnapshot.layout, changedSnapshot.access());
    changedSnapshot.attributes = 0xff;
    ChannelSnapshot unqualified = {};
    unqualified.words[0] = 99;
    expect(snapshotChannel(snapshotMapping, unqualified) == Result::NotReady && unqualified.words[0] == 0 &&
           snapshotMapping.state == MemoryState::Poisoned && changedSnapshot.reads == 0 && changedSnapshot.writes == 0,
           "the first post-exit snapshot revalidates mappings before reading any control word");
    Device failed;
    MemoryBinding broken = {};
    observeMemory(broken, failed.layout, failed.access());
    failed.failRead = true;
    ChannelSnapshot snapshot = {};
    snapshot.words[0] = 99;
    expect(snapshotChannel(broken, snapshot) == Result::IoError && snapshot.words[0] == 0 &&
           broken.state == MemoryState::Poisoned, "failed snapshot exposes no partial data and poisons access");
}

void testIdleHandoff()
{
    for (u32 local = 0; local < 8; ++local) {
        for (u32 remote = 0; remote < 8; ++remote) {
            Device device;
            device.control[0] = local;
            device.control[11] = remote;
            device.control[9] = device.control[10] = 0x60;
            device.control[20] = device.control[21] = 0x28;
            const auto original = device.control;
            MemoryBinding binding = {};
            observeMemory(binding, device.layout, device.access());
            const Result claimed = claimMemory(binding, true, true);
            const bool idle = local == 0 && (remote == 0 || remote == 4);
            expect((claimed == Result::Ok) == idle && device.control == original && device.writes == 0,
                   "only local CLOSED with peer CLOSED/CLOSING and empty measured indices qualifies without writes");
        }
    }
    for (u32 mutation = 0; mutation < 10; ++mutation) {
        Device device;
        device.control[11] = 4;
        device.control[9] = device.control[10] = 0x60;
        device.control[20] = device.control[21] = 0x28;
        switch (mutation) {
        case 0: device.control[1] = 1; break;
        case 1: device.control[12] = 1; break;
        case 2: device.control[10] += 4; break;
        case 3: device.control[21] += 4; break;
        case 4: device.control[9] = device.control[10] = 1024; break;
        case 5: device.control[20] = device.control[21] = 1024; break;
        case 6: device.control[9] = device.control[10] = 1; break;
        case 7: device.control[20] = device.control[21] = 1; break;
        case 8: device.mutateAfterDelay = 1; break;
        case 9: device.mutateAfterDelay = 7; break;
        }
        MemoryBinding binding = {};
        observeMemory(binding, device.layout, device.access());
        expect(claimMemory(binding, true, true) == Result::NotReady && device.writes == 0 &&
               binding.state == MemoryState::Observed,
               "CLOSING peer does not excuse live DTR, unread data, invalid indices or changing metadata");
    }
    for (u32 reply : {1u, 2u}) {
        for (bool closing : {false, true}) {
            Device device;
            device.control[11] = 4;
            device.control[9] = device.control[10] = 0x60;
            device.control[20] = device.control[21] = 0x28;
            device.openingReply = reply;
            device.closingIdle = closing;
            MemoryBinding binding = {};
            SmdPipe pipe = {};
            observeMemory(binding, device.layout, device.access());
            const Result claimed = claimMemory(binding, true, true);
            expect(claimed == Result::Ok, "exclusive post-EBS ownership accepts the photographed idle state pair");
            if (claimed != Result::Ok) { continue; }
            expect(openPipe(pipe, memoryIo(binding), 88, 1024, true, true, 20, 100) == Result::Ok &&
                   device.notifications == 2 && device.control[11] == 2 && device.control[12] == 1 &&
                   device.control[18] == 0,
                   "idle CLOSING peer reopens through either OPENING or OPENED acknowledgement");
            expect(closePipe(pipe, 20, 100) == Result::Ok && pipe.state == PipeState::Closed &&
                   device.control[0] == 0 && device.control[11] == (closing ? 4u : 0u) &&
                   device.control[12] == 0 && device.notifications == 4,
                   "native close accepts a deasserted, empty peer that remains CLOSING after local CLOSED");
            expect(retireMemory(binding, pipe) == Result::Ok && binding.state == MemoryState::Retired,
                   "stable idle channel can retire local access without freeing its shared physical storage");
            for (const auto &operation : device.operations) {
                if (operation.kind != 'S') { continue; }
                expect(operation.address < device.layout.fifo.address ||
                       operation.address >= device.layout.fifo.address + device.layout.fifo.bytes,
                       "CLOSING handoff performs no FIFO payload stores or resource requests");
            }
        }
    }
}

void testOpeningReplies()
{
    Device delayed;
    delayed.openingReply = 1;
    delayed.delayedOpen = true;
    MemoryBinding binding = {};
    SmdPipe pipe = {};
    observeMemory(binding, delayed.layout, delayed.access());
    claimMemory(binding, true, true);
    expect(openPipe(pipe, memoryIo(binding), 88, 1024, true, true, 10, 100) == Result::Ok &&
           delayed.delays == 9 && pipe.state == PipeState::Ready,
           "OPENING acknowledgement is followed by a bounded wait for OPENED and DTR");
    for (u32 failure = 0; failure < 4; ++failure) {
        Device device;
        MemoryBinding memory = {};
        SmdPipe opening = {};
        observeMemory(memory, device.layout, device.access());
        claimMemory(memory, true, true);
        device.control[11] = 4;
        device.peer = failure != 0;
        device.openingReply = failure == 3 ? 5 : 1;
        device.openedReply = failure == 2 ? 5 : 1;
        const Result result = openPipe(opening, memoryIo(memory), 88, 1024, true, true, 7, 100);
        expect(result == (failure < 2 ? Result::TimedOut : Result::IoError) &&
               opening.state == PipeState::Poisoned && device.delays <= 13,
               "both opening phases share one finite deadline and peer RESET is never accepted");
        const unsigned writes = device.writes;
        expect(openPipe(opening, memoryIo(memory), 88, 1024, true, true, 7, 100) == Result::NotReady &&
               device.writes == writes, "failed two-phase opening cannot be silently retried");
    }
}

void testIdleRetirement()
{
    for (u32 mutation = 0; mutation < 9; ++mutation) {
        Device device;
        device.closingIdle = true;
        MemoryBinding binding = {};
        SmdPipe pipe = {};
        observeMemory(binding, device.layout, device.access());
        claimMemory(binding, true, true);
        openPipe(pipe, memoryIo(binding), 88, 1024, true, true, 20, 100);
        closePipe(pipe, 20, 100);
        switch (mutation) {
        case 0: device.control[0] = 2; break;
        case 1: device.control[11] = 1; break;
        case 2: device.control[1] = 1; break;
        case 3: device.control[12] = 1; break;
        case 4: device.control[21] = 4; break;
        case 5: device.control[10] = 4; break;
        case 6: device.attributes = 0xff; break;
        case 7: device.failRead = true; break;
        case 8: device.mutateAfterDelay = device.delays + 7; break;
        }
        const unsigned writes = device.writes;
        expect(retireMemory(binding, pipe) != Result::Ok && binding.state == MemoryState::Poisoned &&
               device.writes == writes,
               "retirement revalidates mappings and eight idle snapshots, refusing late data, DTR or state changes");
        expect(!memoryIo(binding).notify(&binding), "uncertain retirement cannot perform any more native I/O");
    }
}

void testNativeRoundTrip()
{
    for (bool delayed : {false, true}) {
        Device device;
        device.delayedClose = delayed;
        device.control[9] = device.control[10] = 20;
        device.control[20] = device.control[21] = 40;
        MemoryBinding binding = {};
        SmdPipe pipe = {};
        expect(observeMemory(binding, device.layout, device.access()) == Result::Ok &&
               claimMemory(binding, true, true) == Result::Ok && device.delays == 7 && device.writes == 0,
               "closed-channel ownership includes eight stable native snapshots before any write");
        const SmdIo io = memoryIo(binding);
        expect(openPipe(pipe, io, 88, 1024, true, true, 20, 100) == Result::Ok &&
               device.notifications == 2, "actual bounded address callbacks complete a native SMD open");
        const unsigned writesBefore = device.writes;
        u32 value = 0;
        expect(!io.write32(io.context, SmdSpace::Receive, 0, 42) &&
               !io.write32(io.context, SmdSpace::Control, 36, 42) &&
               !io.write32(io.context, SmdSpace::Control, 44, 42) &&
               !io.write32(io.context, SmdSpace::Control, 84, 42) &&
               !io.write32(io.context, SmdSpace::Transmit, 1024, 42) &&
               !io.read32(io.context, SmdSpace::Control, 88, &value) &&
               !io.read32(io.context, static_cast<SmdSpace>(99), 0, &value) &&
               !io.read32(io.context, SmdSpace::Control, 1, &value) && device.writes == writesBefore,
               "adapter refuses remote-owned words, receive FIFO writes and all out-of-range accesses");
        expect(closePipe(pipe, 0, 100) == Result::Invalid && device.writes == writesBefore,
               "invalid close deadline causes no channel change");
        expect(retireMemory(binding, pipe) == Result::NotReady, "open channel mappings cannot be retired");
        expect(closePipe(pipe, 20, 100) == Result::Ok && pipe.state == PipeState::Closed &&
               device.control[0] == 0 && device.control[11] == 0 && device.control[1] == 0 &&
               device.control[12] == 0 && device.control[18] == 0 && device.notifications == 4,
               "native close handles immediate CLOSED and CLOSING peer responses without firmware calls");
        const u8 unused[4] = {};
        expect(sendPacket(pipe, unused, sizeof(unused)) == Transfer::Closed &&
               openPipe(pipe, io, 88, 1024, true, true, 20, 100) == Result::NotReady,
               "closed one-shot channel cannot send traffic or silently reopen");
        expect(retireMemory(binding, pipe) == Result::Ok && binding.state == MemoryState::Retired &&
               !io.notify(io.context) && !io.read32(io.context, SmdSpace::Control, 0, &value) &&
               !io.write32(io.context, SmdSpace::Control, 0, 1), "retired mappings reject all later operations");
        bool sawFifoStore = false;
        for (unsigned index = 0; index < device.operations.size(); ++index) {
            const auto &operation = device.operations[index];
            if (operation.kind != 'S') { continue; }
            if (operation.address >= device.layout.fifo.address &&
                operation.address < device.layout.fifo.address + device.layout.fifo.bytes) { sawFifoStore = true; }
            if (operation.address == device.layout.doorbell.address) {
                expect(index && index + 1 < device.operations.size() &&
                       device.operations[index - 1].kind == 'B' && device.operations[index + 1].kind == 'B',
                       "doorbell store is ordered on both sides by a native barrier");
            }
        }
        expect(!sawFifoStore, "open/close diagnostic never publishes a packet or RPM resource vote");
    }
}

void testFailures()
{
    for (unsigned failure = 0; failure < 5; ++failure) {
        Device device;
        MemoryBinding binding = {};
        SmdPipe pipe = {};
        observeMemory(binding, device.layout, device.access());
        claimMemory(binding, true, true);
        openPipe(pipe, memoryIo(binding), 88, 1024, true, true, 20, 100);
        if (failure == 0) { device.peer = false; }
        if (failure == 1) { device.failWrite = true; }
        if (failure == 2) { device.failBarrier = true; }
        if (failure == 3) { device.remoteReset = true; }
        if (failure == 4) { device.dirtyClose = true; }
        const Result result = closePipe(pipe, 9, 100);
        expect(result != Result::Ok && pipe.state != PipeState::Closed &&
               retireMemory(binding, pipe) == Result::NotReady,
               "failed close, remote reset or late data cannot authorize unmapping the channel");
        if (failure == 0) {
            expect(result == Result::TimedOut && pipe.state == PipeState::Poisoned && device.delays == 15,
                   "silent peer has a finite close deadline and uncertain ownership stays poisoned");
        }
        const unsigned previousWrites = device.writes;
        expect(closePipe(pipe, 9, 100) == Result::NotReady && device.writes == previousWrites,
               "failed close is not automatically retried or converted into success");
    }
}

}

int main()
{
    testMappings(); testOwnership(); testIdleHandoff(); testOpeningReplies(); testIdleRetirement();
    testNativeRoundTrip(); testFailures();
    std::printf("Native SMD memory/handoff: %u checks, %u failures (simulated MMIO; no handset writes)\n", checks, failures);
    return failures ? 1 : 0;
}
