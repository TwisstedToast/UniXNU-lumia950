#include <cstdio>
#include <cstring>
#include <vector>

#include "AdrenoCore.h"
#include "BoardConstants.h"
#include "GpuIo.h"
#include "LpaeCore.h"
#include "PlatformCheck.h"
#include "SmmuContext.h"
#include "Submission.h"

using namespace adreno;

static unsigned failures;
static unsigned checks;

static void expect(bool condition, const char *message)
{
    checks++;
    if (!condition) { std::printf("FAIL %s\n", message); failures++; }
}

struct Write { RegisterSpace space; u32 offset; u32 value; };

struct FakeGpu {
    u32 mmss[0x10000 / 4] = {};
    u32 gpu[0x40000 / 4] = {};
    u32 smmu[0x10000 / 4] = {};
    u32 gpuReads = 0;
    u32 smmuReads = 0;
    u32 barriers = 0;
    u32 cacheCalls = 0;
    u32 delays = 0;
    u32 meWords = 0;
    u32 pfpWords = 0;
    bool failRead = false;
    bool failWrite = false;
    bool failCache = false;
    bool failMapping = false;
    bool execute = true;
    bool corrupt = false;
    std::vector<Write> writes;
    DmaRegion ring = {};
    DmaRegion fence = {};
    DmaRegion destination = {};
    u32 consumed = 0;

    FakeGpu()
    {
        mmss[lumia950::kGdscrGxOffset / 4] = 0x80000000;
        mmss[lumia950::kGdscrCxOffset / 4] = 0x80000000;
        mmss[kCoreClockOffset / 4] = 1;
        mmss[kAhbClockOffset / 4] = 1;
        mmss[kTimerClockOffset / 4] = 1;
        gpu[lumia950::kRegRbbmHwVersion] = 0x04010800;
    }

    void consume(u32 next)
    {
        const u32 mask = ring.bytes / 4 - 1;
        while (consumed != next) {
            const u32 header = ring.cpu[consumed];
            consumed = (consumed + 1) & mask;
            expect((header & 0xc00000ffu) == 0xc0000000u, "independent decoder: PM4 type and reserved bits");
            const u32 count = ((header >> 16) & 0x3fff) + 1;
            const u32 opcode = (header >> 8) & 255;
            expect(count < ring.bytes / 4, "packet count fits the ring");
            if (opcode == 61) {
                const u32 address = ring.cpu[consumed];
                expect(address == destination.gpu, "memory-write target is the owned destination");
                for (u32 index = 1; index < count; index++) {
                    destination.cpu[index - 1] = ring.cpu[(consumed + index) & mask] ^ (corrupt ? 1u : 0u);
                }
            } else if (opcode == 70 && count == 3) {
                expect(ring.cpu[consumed] == 4, "polled fence does not request a hardware IRQ");
                expect(ring.cpu[(consumed + 1) & mask] == fence.gpu, "fence targets owned memory");
                fence.cpu[0] = ring.cpu[(consumed + 2) & mask];
            } else {
                expect(opcode == 72 || opcode == 70 || opcode == 38, "only expected initialization and synchronization commands");
            }
            consumed = (consumed + count) & mask;
        }
        gpu[lumia950::kRegCpRbRptr] = consumed;
    }

    static bool read(void *context, RegisterSpace space, u32 offset, u32 *value)
    {
        FakeGpu &device = *static_cast<FakeGpu *>(context);
        if (device.failRead || !value || (offset & 3)) { return false; }
        if (space == RegisterSpace::Mmss && offset < sizeof(device.mmss)) {
            *value = device.mmss[offset / 4]; return true;
        }
        if (space == RegisterSpace::Gpu && offset < sizeof(device.gpu)) {
            device.gpuReads++;
            *value = device.gpu[offset / 4]; return true;
        }
        if (space == RegisterSpace::Smmu && offset < sizeof(device.smmu)) {
            device.smmuReads++;
            *value = device.smmu[offset / 4]; return true;
        }
        return false;
    }

    static bool write(void *context, RegisterSpace space, u32 offset, u32 value)
    {
        FakeGpu &device = *static_cast<FakeGpu *>(context);
        if (device.failWrite || (offset & 3)) { return false; }
        if (space == RegisterSpace::Smmu && offset < sizeof(device.smmu)) {
            device.writes.push_back({space, offset, value});
            device.smmu[offset / 4] = value;
            return true;
        }
        if (space != RegisterSpace::Gpu || offset >= sizeof(device.gpu)) { return false; }
        device.writes.push_back({space, offset, value});
        device.gpu[offset / 4] = value;
        if (offset == lumia950::kRegCpMeRamData * 4) { device.meWords++; }
        if (offset == lumia950::kRegCpPfpUcodeData * 4) { device.pfpWords++; }
        if (offset == lumia950::kRegCpRbWptr * 4 && value && device.execute) {
            device.consume(value);
        }
        return true;
    }

    static void barrier(void *context) { static_cast<FakeGpu *>(context)->barriers++; }
    static void delay(void *context, u32) { static_cast<FakeGpu *>(context)->delays++; }
    static bool mapped(void *context, const DmaRegion &, bool)
    { return !static_cast<FakeGpu *>(context)->failMapping; }
    static bool cache(void *context, const DmaRegion &, CacheOperation)
    {
        FakeGpu &device = *static_cast<FakeGpu *>(context);
        device.cacheCalls++;
        return !device.failCache;
    }
    static bool cleanTables(void *context, const lpae::Tables &)
    { return !static_cast<FakeGpu *>(context)->failCache; }
    SubmissionIo io()
    { return {{this, read, write, barrier, delay}, mapped, cache}; }
};

alignas(4096) static u32 ringMemory[65536 / 4];
alignas(4096) static u32 fenceMemory[4096 / 4];
alignas(4096) static u32 destinationMemory[4096 / 4];
static const u32 pm4Image[] = {0, 0x4ff065, 2, 3};
static const u32 pfpImage[] = {0, 1, 2, 3, 4, 0x4ff083};

static QueueResult start(FakeGpu &device, Queue &queue, bool ownership = true)
{
    device.ring = {ringMemory, 0x90000000, 0x10000000, sizeof(ringMemory)};
    device.fence = {fenceMemory, 0x90010000, 0x10010000, sizeof(fenceMemory)};
    device.destination = {destinationMemory, 0x90011000, 0x10011000, sizeof(destinationMemory)};
    return initializeQueue(queue, device.io(), device.ring, device.fence,
                           pm4Image, 4, pfpImage, 6, ownership);
}

static void testPower()
{
    FakeGpu device;
    GpuSnapshot snapshot = {};
    expect(readSnapshot(device.io().registers, &snapshot) == ProbeResult::Ready, "warm powered GPU snapshot");
    expect(device.writes.empty(), "snapshot never changes hardware");
    expect(snapshot.version == 0x04010800, "snapshot reads actual version");
    for (u32 offset : {lumia950::kGdscrGxOffset, lumia950::kGdscrCxOffset,
                      kCoreClockOffset, kAhbClockOffset, kTimerClockOffset}) {
        const u32 old = device.mmss[offset / 4];
        device.mmss[offset / 4] = 0;
        device.gpuReads = 0;
        expect(readSnapshot(device.io().registers, &snapshot) != ProbeResult::Ready,
               "collapsed domain or gated branch refuses GPU access");
        expect(device.gpuReads == 0, "no GPU MMIO before every power and clock gate passes");
        device.mmss[offset / 4] = old;
        device.mmss[offset / 4] |= 0x80000001;
        if (offset == lumia950::kGdscrGxOffset || offset == lumia950::kGdscrCxOffset) {
            expect(readSnapshot(device.io().registers, &snapshot) == ProbeResult::CollapsePending,
                   "an in-progress domain transition is refused");
        } else {
            expect(readSnapshot(device.io().registers, &snapshot) == ProbeResult::ClockOff,
                   "clock enable is insufficient while the branch is halted");
        }
        device.mmss[offset / 4] = old;
    }
    device.mmss[kAhbClockOffset / 4] = 0xffffffff;
    expect(readSnapshot(device.io().registers, &snapshot) == ProbeResult::NoResponse, "all-ones clock read is not ready");
    device.failRead = true;
    expect(readSnapshot(device.io().registers, &snapshot) == ProbeResult::IoError, "MMIO failures propagate");
    FakeGpu handset;
    handset.mmss[lumia950::kGdscrGxOffset / 4] = 0x00282001;
    handset.mmss[lumia950::kGdscrCxOffset / 4] = 0x00282001;
    handset.mmss[kCoreClockOffset / 4] = 0x80000ff0;
    handset.mmss[kAhbClockOffset / 4] = 0x00008000;
    handset.mmss[kTimerClockOffset / 4] = 0x80000000;
    expect(readSnapshot(handset.io().registers, &snapshot) == ProbeResult::DomainOff,
           "2026-10-26 handset photograph is classified as both domains off");
    expect(handset.gpuReads == 0 && handset.writes.empty(),
           "photographed power state never permits GPU reads or hardware writes");
    expect(snapshot.power.gx == 0x00282001 && snapshot.power.cx == 0x00282001 &&
           snapshot.power.coreClock == 0x80000ff0 && snapshot.power.ahbClock == 0x00008000 &&
           snapshot.power.timerClock == 0x80000000, "all photographed MMSS values survive the refusal");
    snapshot.power.gx = snapshot.power.cx = 0x80000000;
    expect(checkPower(snapshot.power) == ProbeResult::ClockOff,
           "power-on alone would not qualify the photographed halted branches");
    snapshot.power.coreClock = snapshot.power.timerClock = 1;
    expect(checkPower(snapshot.power) == ProbeResult::ClockOff,
           "AHB halt bit clear does not mean its software enable bit is set");
    handset.mmss[kAhbClockOffset / 4] = 0x80008000;
    expect(readSnapshot(handset.io().registers, &snapshot) == ProbeResult::DomainOff,
           "v2 handset report with halted AHB still refuses the powered-off GPU");
    expect(snapshot.power.ahbClock == 0x80008000 && snapshot.power.gx == 0x00282001 &&
           snapshot.power.cx == 0x00282001, "v2 hardware values are preserved rather than copied from v1");
    expect(handset.gpuReads == 0 && handset.smmuReads == 0 && handset.writes.empty(),
           "neither photographed state permits GPU or SMMU reads or hardware writes");
    snapshot.power.gx = snapshot.power.cx = 0x80000000;
    expect(checkPower(snapshot.power) == ProbeResult::ClockOff,
           "power-on alone would still leave all three photographed v2 branches unqualified");
}

static void testMappings()
{
    alignas(4096) static u64 root[512];
    alignas(4096) static u64 pool[8 * 512];
    std::memset(root, 0, sizeof(root));
    std::memset(pool, 0, sizeof(pool));
    lpae::Tables tables = {root, 0x80000000, pool, 0x80001000, 8, 0};
    expect(lpae::validTables(tables), "LPAE memory belongs to distinct aligned regions");
    expect(!lpae::mapRange(tables, 0x30000000, tables.rootPhysical, 4096, true, false),
           "GPU cannot map and overwrite its own translation root");
    expect(!lpae::mapRange(tables, 0x30000000, tables.poolPhysical, 4096, false, false),
           "page-table pool is not exposed as a GPU data buffer");
    expect(lpae::mapRange(tables, 0x10000000, 0x90000000, 0x10000, false, false), "map ring in owned GPU address space");
    expect(tables.usedPages == 2, "ring allocates one level-2 and one level-3 table");
    expect(root[0] == 0x8000000080001003ull, "root descriptor matches nonsecure ARM LPAE encoding");
    expect(pool[0x80] == 0x8000000080002003ull, "middle descriptor points to the owned nonsecure leaf");
    expect(pool[512] == 0x00600000900004e7ull, "read-only NC leaf matches independent ARM bit layout");
    expect(lpae::matches(tables, 0x10000000, 0x90000000, 0x10000, false), "all ring pages resolve to expected physical memory");
    expect(!lpae::matches(tables, 0x10000000, 0x90000000, 0x10000, true), "GPU cannot write into a read-only ring mapping");
    expect(lpae::mapRange(tables, 0x10010000, 0x90010000, 0x2000, true, false), "map fence and destination writable");
    lpae::Mapping mapping = {};
    expect(lpae::lookup(tables, 0x10011234, &mapping) && mapping.physical == 0x90011234 &&
           mapping.writable && !mapping.cacheable, "lookup preserves byte offset and permissions");
    expect(!lpae::mapRange(tables, 0x10011000, 0x91000000, 0x2000, true, false), "overlapping map is refused atomically");
    expect(!lpae::lookup(tables, 0x10012000, &mapping), "refused map creates no partial page");
    expect(!lpae::mapRange(tables, 0xfffff000, 0x90000000, 0x2000, true, false), "32-bit IOVA wrap refused");
    expect(!lpae::mapRange(tables, 0x20000000, 0xfffff000, 0x2000, true, false), "32-bit physical wrap refused");
    expect(!lpae::mapRange(tables, 1, 0x90000000, 4096, true, false), "unaligned IOVA refused");
    expect(!lpae::unmapRange(tables, 0x10010000, 0x3000), "unmap containing a hole is refused atomically");
    expect(lpae::lookup(tables, 0x10010000, &mapping), "failed unmap retains earlier mappings");
    expect(lpae::unmapRange(tables, 0x10010000, 0x2000), "unmap owned range");
    expect(!lpae::lookup(tables, 0x10010000, &mapping), "unmapped address no longer resolves");
    const u32 before = tables.usedPages;
    tables.poolPages = before;
    expect(!lpae::mapRange(tables, 0x1ffff000, 0x91000000, 0x2000, true, false), "pool exhaustion across a table boundary is atomic");
    expect(tables.usedPages == before && !lpae::lookup(tables, 0x1ffff000, &mapping), "failed allocation leaves no mapping or leaked table");
    tables.poolPages = 8;
    expect(lpae::mapRange(tables, 0x3ffff000, 0x91000000, 0x2000, true, true), "mapping crosses a level-1 boundary");
    expect(lpae::lookup(tables, 0x40000000, &mapping) && mapping.cacheable && mapping.physical == 0x91001000,
           "cross-root page and cacheability decode correctly");
    expect(lpae::mapRange(tables, 0xfffff000, 0xfffff000, 0x1000, true, false), "last 32-bit page maps without overflow");
    expect(lpae::lookup(tables, 0xffffffff, &mapping) && mapping.physical == 0xffffffff, "last byte remains addressable");
    tables.poolPhysical = tables.rootPhysical;
    expect(!lpae::validTables(tables), "overlapping physical table ownership refused");
}

static void testQueue()
{
    FakeGpu device;
    Queue queue = {};
    expect(start(device, queue) == QueueResult::Ok, "load both microcodes and submit CP_ME_INIT");
    expect(device.meWords == 3 && device.pfpWords == 5, "firmware container header is skipped and no image words are lost");
    expect(ringMemory[0] == 0xc0104800 && ringMemory[15] == 0x20000000,
           "A418 ME_INIT enables protected-register handling as Qualcomm specifies");
    expect(device.writes.front().offset == lumia950::kRegCpMeCntl * 4 &&
           device.writes.front().value == kMeHalt, "micro engine is halted before firmware writes");
    expect(waitQueue(queue, 10, 1) == QueueResult::Ok && queue.state == QueueState::Ready, "init requires a real fence and drained ring");
    expect(verifyFill(queue, device.destination, 0, 1) == QueueResult::Invalid, "initialization fence alone is not proof of a fill");
    expect(submitFill(queue, device.destination, 0xff00aa55, 256) == QueueResult::Ok, "submit a bounded GPU memory fill");
    expect(waitQueue(queue, 10, 1) == QueueResult::Ok, "GPU fill completes with polled fence");
    expect(verifyFill(queue, device.destination, 0xff00aa55, 256) == QueueResult::Ok, "verify every destination dword");
    expect(verifyFill(queue, device.destination, 0xff00aa55, 255) == QueueResult::Invalid, "verification cannot substitute a shorter workload");
    expect(destinationMemory[256] == 0, "GPU test does not write past its destination workload");
    expect(submitFill(queue, device.ring, 0, 1) == QueueResult::Invalid, "GPU cannot overwrite its own ring");
    expect(submitFill(queue, device.fence, 0, 1) == QueueResult::Invalid, "GPU fill cannot forge its completion fence");
    expect(submitFill(queue, device.destination, 0, 257) == QueueResult::Invalid, "oversized test fill is refused");
    queue.writePointer = device.ring.bytes / 4 - 4;
    device.consumed = queue.writePointer;
    device.gpu[lumia950::kRegCpRbRptr] = queue.writePointer;
    expect(submitFill(queue, device.destination, 0x12345678, 16) == QueueResult::Ok, "submission wraps a packet across the ring boundary");
    expect(waitQueue(queue, 10, 1) == QueueResult::Ok, "wrapped submission fence completes");
    expect(verifyFill(queue, device.destination, 0x12345678, 16) == QueueResult::Ok, "wrapped command data verifies");
    queue.sequence = 0xffffffff;
    fenceMemory[0] = 0xffffffff;
    expect(submitFill(queue, device.destination, 0xabcdef01, 1) == QueueResult::Ok, "fence counter wraps");
    expect(waitQueue(queue, 10, 1) == QueueResult::Ok && queue.sequence == 0, "wrapped sequence completes");
    expect(haltQueue(queue) == QueueResult::Ok && !queue.ownsHardware, "successful halt relinquishes ownership");
    expect(submitFill(queue, device.destination, 0, 1) == QueueResult::NotReady, "halted queue cannot submit");
    expect(device.barriers && device.cacheCalls, "DMA paths issue synchronization callbacks");
}

static void testRefusals()
{
    for (u32 scenario = 0; scenario < 11; scenario++) {
        FakeGpu device;
        Queue queue = {};
        if (scenario == 0) { device.failMapping = true; }
        if (scenario == 1) { device.mmss[lumia950::kGdscrGxOffset / 4] = 0; }
        if (scenario == 2) { device.gpu[lumia950::kRegRbbmStatus] = kGpuBusy; }
        if (scenario == 3) { device.failRead = true; }
        if (scenario == 4) { device.gpu[kInterruptStatusIndex] = 0x200; }
        if (scenario == 5) { device.failWrite = true; }
        if (scenario == 6) { device.failCache = true; }
        if (scenario == 7) { device.gpu[lumia950::kRegCpRbRptr] = 2; device.gpu[lumia950::kRegCpRbWptr] = 2; }
        if (scenario == 8) { device.mmss[kAhbClockOffset / 4] = 0x80000001; }
        if (scenario == 9) { queue.state = QueueState::Failed; }
        expect(start(device, queue, scenario != 10) != QueueResult::Ok, "failed prerequisite refuses initialization");
        if (scenario != 6) { expect(device.writes.empty(), "failed preflight performs no hardware writes"); }
    }
    FakeGpu stalled;
    Queue queue = {};
    stalled.execute = false;
    expect(start(stalled, queue) == QueueResult::Ok, "timeout fixture starts without fake execution");
    expect(waitQueue(queue, 5, 1) == QueueResult::TimedOut, "lost fence times out");
    expect(queue.polls == 5 && queue.state == QueueState::Failed, "timeout consumes exact budget and poisons queue");
    expect(submitFill(queue, stalled.destination, 0, 1) == QueueResult::NotReady, "timed-out queue cannot recycle memory through another submission");
    expect(haltQueue(queue) == QueueResult::Ok, "explicit halt is required after a timeout");
    FakeGpu faulted;
    queue = {};
    faulted.execute = false;
    expect(start(faulted, queue) == QueueResult::Ok, "fault fixture starts");
    faulted.gpu[kInterruptStatusIndex] = 0x200;
    expect(waitQueue(queue, 10, 1) == QueueResult::GpuFault && queue.fault == 0x200, "opcode fault is retained without false completion");
    FakeGpu corrupt;
    queue = {};
    expect(start(corrupt, queue) == QueueResult::Ok && waitQueue(queue, 10, 1) == QueueResult::Ok, "corruption fixture initializes");
    corrupt.corrupt = true;
    expect(submitFill(queue, corrupt.destination, 0xaa55aa55, 8) == QueueResult::Ok &&
           waitQueue(queue, 10, 1) == QueueResult::Ok, "a fence can complete even with incorrect data");
    expect(verifyFill(queue, corrupt.destination, 0xaa55aa55, 8) == QueueResult::DataMismatch,
           "data verification rejects a fence-only false positive");
    for (u32 scenario = 0; scenario < 4; scenario++) {
        FakeGpu device;
        Queue attempt = {};
        expect(start(device, attempt) == QueueResult::Ok && waitQueue(attempt, 5, 1) == QueueResult::Ok,
               "runtime failure fixture initializes");
        if (scenario == 0) { device.failMapping = true; }
        if (scenario == 1) { device.failRead = true; }
        if (scenario == 2) { device.failCache = true; }
        if (scenario == 3) { device.gpu[lumia950::kRegCpRbRptr] = attempt.ring.bytes / 4; }
        const size_t writesBefore = device.writes.size();
        expect(submitFill(attempt, device.destination, 0x10101010, 2) != QueueResult::Ok,
               "runtime mapping, cache or pointer failure cannot submit");
        expect(device.writes.size() == writesBefore, "runtime preflight does not ring the doorbell on failure");
    }
    FakeGpu full;
    queue = {};
    expect(start(full, queue) == QueueResult::Ok && waitQueue(queue, 5, 1) == QueueResult::Ok,
           "ring-capacity fixture initializes");
    full.gpu[lumia950::kRegCpRbRptr] = queue.writePointer + 1;
    expect(submitFill(queue, full.destination, 0, 1) == QueueResult::NoRoom &&
           queue.state == QueueState::Ready, "ring capacity is bounded without advancing queue state");
}

static void testContexts()
{
    alignas(4096) static u64 root[512];
    alignas(4096) static u64 pool[2 * 512];
    std::memset(root, 0, sizeof(root));
    std::memset(pool, 0, sizeof(pool));
    const lpae::Tables tables = {root, 0x80000000, pool, 0x80001000, 2, 0};
    FakeGpu device;
    device.gpu[lumia950::kRegCpMeCntl] = kMeHalt;
    ContextIo io = {device.io().registers, FakeGpu::cleanTables};
    ContextBank bank = {};
    expect(bindDisabledContext(bank, io, tables, 0, 1, true) == ContextResult::Ok,
           "configure owned disabled user bank with the engine halted");
    expect(device.smmu[0x8020 / 4] == 0x80000000 && device.smmu[0x8024 / 4] == 0x10000,
           "TTBR0 contains physical root and ASID in the correct halves");
    expect(device.smmu[0x8030 / 4] == 0x80802020 && device.smmu[0x8010 / 4] == 0x38000,
           "TCR selects 32-bit IOVA, 4K LPAE, NC outer-shareable walk and no TTBR1");
    expect(device.smmu[0x8038 / 4] == 0xff4404 && device.smmu[0x8000 / 4] == 0x10a7,
           "MAIR and noninterrupting fault-report/stall controls match the register layout");
    device.smmu[0x8050 / 4] = 0x90000000;
    device.smmu[0x8054 / 4] = 0x44000000;
    expect(checkTranslation(bank, 0x10000123, 0x90000123, 3) == ContextResult::Ok,
           "hardware translation verification ignores PAR memory attributes but preserves the physical page");
    expect(checkTranslation(bank, 0x10000000, 0x91000000, 3) == ContextResult::Mismatch,
           "incorrect hardware translation cannot qualify DMA");
    device.smmu[0x8050 / 4] = 1;
    expect(checkTranslation(bank, 0x10000000, 0x90000000, 3) == ContextResult::TranslationFault,
           "PAR fault is not an address");
    device.smmu[0x88f0 / 4] = 1;
    expect(checkTranslation(bank, 0x10000000, 0x90000000, 3) == ContextResult::TimedOut,
           "ATOS translation waits are bounded");
    device.smmu[0x88f0 / 4] = 0;
    device.smmu[0x87f4 / 4] = 1;
    expect(disableContext(bank, 3) == ContextResult::TimedOut && bank.ownsBank,
           "unsynchronized disable pins tables instead of freeing DMA-visible memory");
    device.smmu[0x87f4 / 4] = 0;
    expect(disableContext(bank, 3) == ContextResult::Ok && !bank.ownsBank,
           "successful invalidation and disable release bank ownership");
    bank = {};
    expect(bindDisabledContext(bank, io, tables, 1, 2, true) == ContextResult::Ok,
           "privileged context uses its own bank and ASID");
    device.gpu[lumia950::kRegCpMeCntl] = 0;
    const size_t beforeDisable = device.writes.size();
    expect(disableContext(bank, 3) == ContextResult::NotReady && bank.ownsBank &&
           device.writes.size() == beforeDisable, "a running engine cannot lose its SMMU mapping");
    device.gpu[lumia950::kRegCpMeCntl] = kMeHalt;
    expect(disableContext(bank, 3) == ContextResult::Ok, "halt permits context disable");
    for (u32 index = 0; index < 6; index++) {
        FakeGpu invalid;
        invalid.gpu[lumia950::kRegCpMeCntl] = kMeHalt;
        ContextBank attempt = {};
        ContextIo invalidIo = {invalid.io().registers, FakeGpu::cleanTables};
        if (index == 0) { invalid.smmu[0x8000 / 4] = 1; }
        if (index == 1) { invalid.gpu[lumia950::kRegCpMeCntl] = 0; }
        if (index == 2) { invalid.failCache = true; }
        if (index == 3) { invalid.smmu[0x8058 / 4] = 2; }
        expect(bindDisabledContext(attempt, invalidIo, tables, index == 4 ? 2 : 0, 1,
                                  index != 5) != ContextResult::Ok,
               "active, unowned, faulted, running, or secure/spare bank is refused");
        expect(invalid.writes.empty(), "context preflight errors write no registers");
    }
}

static void store32(u8 *destination, u32 value)
{
    for (u32 index = 0; index < 4; index++) { destination[index] = static_cast<u8>(value >> (index * 8)); }
}

static void updateChecksum(std::vector<u8> &bytes)
{
    bytes[9] = 0;
    u8 sum = 0;
    for (u8 value : bytes) { sum = static_cast<u8>(sum + value); }
    bytes[9] = static_cast<u8>(0u - sum);
}

static void testTimerPlatform()
{
    std::vector<u8> table(156);
    store32(table.data(), 0x54445447);
    store32(table.data() + 4, static_cast<u32>(table.size()));
    store32(table.data() + 64, 20);
    updateChecksum(table);
    expect(isLumia950Gtdt(table.data(), static_cast<u32>(table.size())), "Lumia virtual timer GSI 20 is independently checked");
    for (u32 interrupt = 0; interrupt < 64; ++interrupt) {
        auto changed = table;
        store32(changed.data() + 64, interrupt);
        updateChecksum(changed);
        expect(isLumia950Gtdt(changed.data(), static_cast<u32>(changed.size())) == (interrupt == 20),
            "wrong-platform virtual timer IRQ is refused before GIC writes");
    }
    for (u32 flags : {0u, 1u, 2u, 3u, 4u, 8u, 0xffffffffu}) {
        auto changed = table;
        store32(changed.data() + 68, flags);
        updateChecksum(changed);
        expect(isLumia950Gtdt(changed.data(), static_cast<u32>(changed.size())) == (flags == 0 || flags == 4),
            "only level-high timer with optional always-on flag is accepted");
    }
    for (u32 bytes = 0; bytes < 80; ++bytes) {
        expect(!isLumia950Gtdt(table.data(), bytes), "truncated timer description is refused");
    }
    auto corrupt = table;
    corrupt[9] ^= 1;
    expect(!isLumia950Gtdt(corrupt.data(), static_cast<u32>(corrupt.size())), "bad timer table checksum is refused");
    corrupt = table;
    corrupt[0] = 'A';
    updateChecksum(corrupt);
    expect(!isLumia950Gtdt(corrupt.data(), static_cast<u32>(corrupt.size())), "another ACPI table cannot qualify the timer");
    expect(!isLumia950Gtdt(nullptr, 156), "missing timer table is refused");
}

static void testPlatform()
{
    std::vector<u8> table(44 + 6 * 80 + 24);
    store32(table.data(), 0x43495041);
    store32(table.data() + 4, static_cast<u32>(table.size()));
    store32(table.data() + 36, 0xf9002000);
    const u32 affinities[] = {0, 1, 2, 3, 0x100, 0x101};
    for (u32 index = 0; index < 6; index++) {
        u8 *entry = table.data() + 44 + index * 80;
        entry[0] = 11; entry[1] = 80;
        store32(entry + 12, 1);
        store32(entry + 32, 0xf9002000);
        store32(entry + 68, affinities[index]);
    }
    u8 *distributor = table.data() + 44 + 6 * 80;
    distributor[0] = 12; distributor[1] = 24;
    store32(distributor + 8, 0xf9000000); distributor[20] = 2;
    updateChecksum(table);
    expect(isLumia950Madt(table.data(), static_cast<u32>(table.size())), "six MSM8992 affinities and GICv2 pass the platform gate");
    for (u32 index = 0; index < 9; index++) {
        auto mutant = table;
        if (index == 0) { mutant[0] = 0; }
        if (index == 1) { mutant[44 + 1] = 0; }
        if (index == 2) { mutant[44 + 80 + 68] = 0; }
        if (index == 3) { mutant[44 + 32] = 1; }
        if (index == 4) { mutant[44 + 12] = 0; }
        if (index == 5) { mutant[44 + 6 * 80 + 20] = 3; }
        if (index == 6) { mutant[44 + 6 * 80 + 8] = 1; }
        if (index == 7) { mutant[44 + 5 * 80 + 68] = 7; }
        updateChecksum(mutant);
        if (index == 8) { mutant[9]++; }
        expect(!isLumia950Madt(mutant.data(), static_cast<u32>(mutant.size())),
               "wrong SoC, malformed entry, duplicate CPU or bad checksum refuses MMIO");
    }
    expect(!isLumia950Madt(nullptr, 0), "missing firmware tables refuse MMIO");
    expect(!isLumia950Madt(table.data(), 40), "truncated firmware table refuses MMIO");
}

int main(int argumentCount, char **arguments)
{
    testPower(); testMappings(); testQueue(); testRefusals(); testContexts(); testPlatform(); testTimerPlatform();
    if (argumentCount == 2) {
        std::FILE *fixture = std::fopen(arguments[1], "rb");
        std::vector<u8> bytes(65536);
        const size_t count = fixture ? std::fread(bytes.data(), 1, bytes.size(), fixture) : 0;
        if (fixture) { std::fclose(fixture); }
        expect(count > 0 && isLumia950Madt(bytes.data(), static_cast<u32>(count)),
               "the upstream Lumia950XLPkg MADT passes the board gate");
    }
    std::printf("GPU backend: %u checks, %u failures (simulated MMIO, not hardware evidence)\n", checks, failures);
    return failures ? 1 : 0;
}
