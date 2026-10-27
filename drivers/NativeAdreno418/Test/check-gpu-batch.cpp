#include "GpuBatch.h"
#include "BoardConstants.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace adreno;

namespace {
unsigned checks;
unsigned failures;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", message); }
}

enum class Fault {
    None, SmemTranslation, PeerSilent, AckLost, AckMalformed, AckError, AckStale,
    GxOn, BranchOn, SpTpOn, Protection, SmmuGlobalAbort, RouteBypass, RouteAlias, ContextEnabled,
    ContextReadback, TlbBusy, AtsFault, AtsWrongPhysical, SecondContextAts,
    CpStall, BadFence, CorruptFill, DestinationGuard, FenceGuard, HaltBusy,
    DisableContext, SpTpOff, GxOff, ReleaseAckLost, CloseSilent, Retirement, AtsWriteFault, Retention
};

struct Device {
    alignas(4096) std::array<u64, 512> root = {};
    alignas(4096) std::array<u64, 8 * 512> pool = {};
    alignas(4096) std::array<u32, 16384> ring = {};
    alignas(4096) std::array<u32, 1024> fence = {};
    alignas(4096) std::array<u32, 1024> destination = {};
    std::array<u64, 512> deviceRoot = {};
    std::array<u64, 8 * 512> devicePool = {};
    std::array<u32, 16384> deviceRing = {};
    std::array<u32, 1024> deviceFence = {};
    std::array<u32, 1024> deviceDestination = {};
    std::array<u32, 0x10000 / 4> mmss = {};
    std::array<u32, 0x10000 / 4> gpu = {};
    std::array<u32, 0x10000 / 4> smmu = {};
    std::array<u32, 22> control = {};
    std::array<u32, 512> fifo = {};
    std::array<u32, 4> votes = {1, 19200, 19200, 0};
    std::vector<u32> requestIds;
    std::vector<u32> requestedResources;
    std::vector<u32> requestedValues;
    GpuBatch batch = {};
    BatchPlan plan = {};
    BatchBuffers buffers = {};
    BatchBudget budget = {24, 1, 16, 16};
    Fault fault = Fault::None;
    u32 failWrite = 0;
    u32 writes = 0;
    u32 gpuReads = 0;
    u32 smmuReads = 0;
    u32 memoryWrites = 0;
    u32 cacheCleans = 0;
    u32 cacheInvalidates = 0;
    u32 tableCleans = 0;
    u32 translations[2] = {};
    u32 meWords = 0;
    u32 pfpWords = 0;
    u32 submissions = 0;
    u32 fills = 0;
    u32 consumed = 0;
    bool closed = false;
    bool violation = false;
    bool releaseBeforeQuiescence = false;
    static constexpr u32 pm4[] = {0, 0x4ff065, 2, 3};
    static constexpr u32 pfp[] = {0, 1, 2, 3, 4, 0x4ff083};

    Device()
    {
        plan.memory = {{rpm::kMessageRamPhysical, 0x100000, 88},
            {rpm::kMessageRamPhysical + 0x200, 0x100200, 2048},
            {rpm::kRpmDoorbellPhysical, 0x200008, 4}};
        const rpm::Resource resources[] = {{0x54534554, 0}, rpm::kMmssAhbParent,
            {0x54534554, 1}, rpm::kGraphicsParent};
        const u32 requested[] = {2, 50000, 100000, 27000};
        for (u32 index = 0; index < 4; ++index) {
            plan.resources[index] = {resources[index], rpm::Set::Active,
                {{rpm::kRateKey, votes[index]}}, 1, true};
            plan.power.votes[index] = {static_cast<power::VoteRole>(index), index,
                {{rpm::kRateKey, requested[index]}}, 1};
        }
        plan.resourceCount = 4;
        plan.power.count = 4;
        plan.power.owner = 17;
        plan.power.maximumGraphicsKHz = 27000;
        plan.power.boardProfileQualified = true;
        plan.power.exclusiveHardware = true;
        plan.hardware = {lumia950::kChipId, true, true};
        plan.routes = {true, true, true};
        plan.firstMessageId = 0x418000;
        plan.fillValue = 0x27c0ffee;
        plan.fillDwords = 256;
        plan.firmwareExited = plan.exclusiveChannel = plan.messageEpochQualified = true;
        plan.baselineQualified = plan.dmaOwnershipQualified = plan.firmwareDigestsQualified = true;
        buffers = {{root.data(), 0x88000000, pool.data(), 0x88001000, 8, 0},
            {ring.data(), 0x90000000, 0x10000000, 65536},
            {fence.data(), 0x90010000, 0x10010000, 4096},
            {destination.data(), 0x90011000, 0x10011000, 4096}, pm4, 4, pfp, 6};
        mmss[0x4024 / 4] = mmss[0x4034 / 4] = 0x00282001;
        mmss[0x4028 / 4] = 0x80000ff0;
        mmss[0x403c / 4] = 0x80008000;
        mmss[0x40b0 / 4] = 0x80000000;
        gpu[0] = 0x04010800;
        smmu[0] = 1u << 10;
        smmu[0x20 / 4] = (1u << 30) | (1u << 27) | (1u << 26) | 4;
        smmu[0x24 / 4] = (2u << 28) | 8;
        smmu[0x28 / 4] = 1u << 12;
        smmu[0x800 / 4] = 0x80000000;
        smmu[0x804 / 4] = 0x80000001;
        smmu[0xc00 / 4] = 0;
        smmu[0xc04 / 4] = 1;
        smmu[0x1000 / 4] = smmu[0x1004 / 4] = 1u << 16;
        control[11] = 4;
        control[12] = 1;
    }

    bool powered() const
    {
        return (mmss[0x4024 / 4] & 0x80000001u) == 0x80000000u &&
            (mmss[0x4034 / 4] & 0x80000001u) == 0x80000000u &&
            (mmss[0x4028 / 4] & 0x80000001u) == 1 &&
            (mmss[0x403c / 4] & 0x80000001u) == 1 &&
            (mmss[0x40b0 / 4] & 0x80000001u) == 1;
    }

    bool allVotes() const
    { return votes[0] >= 2 && votes[1] >= 50000 && votes[2] >= 100000 && votes[3] >= 27000; }

    bool quiescent() const
    {
        return (gpu[0x22d] & (1u << 28)) && (gpu[0x191] & 0x80000000u) == 0 &&
            (smmu[0x8000 / 4] & 1) == 0 && (smmu[0x9000 / 4] & 1) == 0 &&
            (gpu[0x1b0] & (1u << 20)) == 0 &&
            (mmss[0x4024 / 4] & 0x80000000u) == 0 && (mmss[0x4034 / 4] & 0x80000000u) == 0;
    }

    u64 tableWord(u64 physical) const
    {
        if (physical >= 0x88000000 && physical < 0x88001000) {
            return deviceRoot[static_cast<std::size_t>((physical - 0x88000000) / 8)];
        }
        if (physical >= 0x88001000 && physical < 0x88009000) {
            return devicePool[static_cast<std::size_t>((physical - 0x88001000) / 8)];
        }
        return 0;
    }

    u64 translated(u32 bank, u32 address, bool writing) const
    {
        const u32 offset = 0x8000 + bank * 0x1000;
        if ((smmu[offset / 4] & 1) == 0) { return 0; }
        const u64 rootPhysical = smmu[(offset + 0x20) / 4];
        const u64 first = tableWord(rootPhysical + (address >> 30) * 8);
        if ((first & 3) != 3) { return 0; }
        const u64 second = tableWord((first & 0xfffff000ull) + ((address >> 21) & 511) * 8);
        if ((second & 3) != 3) { return 0; }
        const u64 leaf = tableWord((second & 0xfffff000ull) + ((address >> 12) & 511) * 8);
        if ((leaf & 3) != 3 || (leaf & (1ull << 10)) == 0 || (writing && (leaf & (1ull << 7)))) { return 0; }
        return (leaf & 0xfffff000ull) | (address & 4095);
    }

    u32 *dmaWord(u32 address, bool writing)
    {
        const u64 physical = translated(0, address, writing);
        if (physical >= 0x90000000 && physical < 0x90010000 && !writing) {
            return &deviceRing[static_cast<std::size_t>((physical - 0x90000000) / 4)];
        }
        if (physical >= 0x90010000 && physical < 0x90011000) {
            return &deviceFence[static_cast<std::size_t>((physical - 0x90010000) / 4)];
        }
        if (physical >= 0x90011000 && physical < 0x90012000) {
            return &deviceDestination[static_cast<std::size_t>((physical - 0x90011000) / 4)];
        }
        violation = true;
        gpu[0x17d] |= 1u << 5;
        return nullptr;
    }

    bool readRing(u32 index, u32 &value)
    {
        u32 *word = dmaWord(buffers.ring.gpu + (index & 16383) * 4, false);
        if (!word) { return false; }
        value = *word;
        return true;
    }

    void execute(u32 next)
    {
        ++submissions;
        if (fault == Fault::CpStall) { return; }
        if (!powered() || !allVotes() || !meWords || !pfpWords ||
            gpu[0x22e] != 0x02004000 || gpu[0x24e] != 0x6e010000 ||
            gpu[0x250] != 7 || gpu[0x37] != 0 || (gpu[0x22d] & (1u << 28))) {
            violation = true; return;
        }
        u32 total = 0;
        while (consumed != next && total++ < 1024) {
            u32 header = 0;
            if (!readRing(consumed++, header)) { return; }
            consumed &= 16383;
            const u32 count = ((header >> 16) & 0x3fff) + 1;
            const u32 opcode = (header >> 8) & 255;
            if ((header & 0xc00000ffu) != 0xc0000000u || count > 257) { violation = true; return; }
            std::array<u32, 257> words = {};
            for (u32 index = 0; index < count; ++index) {
                if (!readRing(consumed + index, words[index])) { return; }
            }
            if (opcode == 72) {
                const u32 expected[] = {0x3f7, 0, 0, 0, 0x80, 0x100, 0x180, 0x6600,
                    0x150, 0x14e, 0x154, 1, 0, 0, 0x20000000, 0, 0};
                if (count != 17 || std::memcmp(words.data(), expected, sizeof(expected))) { violation = true; return; }
            } else if (opcode == 61) {
                ++fills;
                if (words[0] != buffers.destination.gpu || count != plan.fillDwords + 1) { violation = true; return; }
                for (u32 index = 1; index < count; ++index) {
                    u32 *target = dmaWord(words[0] + (index - 1) * 4, true);
                    if (!target) { return; }
                    *target = words[index] ^ (fault == Fault::CorruptFill ? 1u : 0u);
                }
                if (fault == Fault::DestinationGuard) { deviceDestination[plan.fillDwords] ^= 1; }
            } else if (opcode == 70 && count == 3) {
                if (words[0] != 4 || words[1] != buffers.fence.gpu) { violation = true; return; }
                u32 *target = dmaWord(words[1], true);
                if (!target) { return; }
                *target = words[2] + (fault == Fault::BadFence ? 1u : 0u);
                if (fault == Fault::FenceGuard) { deviceFence[1] ^= 1; }
            } else if (!((opcode == 70 && count == 1 && words[0] == 7) ||
                         (opcode == 38 && count == 1 && words[0] == 0))) { violation = true; return; }
            consumed = (consumed + count) & 16383;
        }
        if (total >= 1024) { violation = true; return; }
        gpu[0x204] = consumed;
    }

    static bool read(void *context, RegisterSpace space, u32 offset, u32 *value)
    {
        auto &device = *static_cast<Device *>(context);
        if (!value || (offset & 3) || offset >= 0x10000) { return false; }
        if (space == RegisterSpace::Mmss) { *value = device.mmss[offset / 4]; return true; }
        if (!device.powered()) { device.violation = true; return false; }
        if (space == RegisterSpace::Smmu && device.fault == Fault::SmmuGlobalAbort && offset == 0x20) {
            return false;
        }
        if (space == RegisterSpace::Gpu) {
            ++device.gpuReads;
            *value = device.gpu[offset / 4];
            if (device.fault == Fault::Protection && offset == 0x240 * 4) { *value ^= 1; }
        } else {
            ++device.smmuReads;
            *value = device.smmu[offset / 4];
            if (device.fault == Fault::RouteBypass && offset == 0xc00) { *value = 1u << 16; }
            if (device.fault == Fault::RouteAlias && offset == 0x808) { *value = 0x80000002; }
            if (device.fault == Fault::ContextEnabled && offset == 0x8000) { *value |= 1; }
            if (device.fault == Fault::ContextReadback && offset == 0x8000 && (*value & 1)) { *value ^= 1; }
            if (device.fault == Fault::TlbBusy && (offset == 0x87f4 || offset == 0x97f4)) { *value = 1; }
        }
        return true;
    }

    static bool write(void *context, RegisterSpace space, u32 offset, u32 value)
    {
        auto &device = *static_cast<Device *>(context);
        if (++device.writes == device.failWrite || (offset & 3) || offset >= 0x10000) { return false; }
        if (space == RegisterSpace::Mmss) {
            if ((offset == 0x4024 || offset == 0x4034) && !(value & 1)) {
                if (!device.allVotes()) { device.violation = true; return false; }
                if (offset == 0x4024 && !(device.mmss[0x4034 / 4] & 0x80000000u)) {
                    device.violation = true; return false;
                }
            }
            if (offset == 0x4024 || offset == 0x4034) {
                if ((value & 1) && device.batch.verifiedPasses != 2) { device.violation = true; }
                value = (value & 1) ? value & ~0x80000000u : value | 0x80000000u;
                if (offset == 0x4024 && device.fault == Fault::GxOn) { value &= ~0x80000000u; }
                if (offset == 0x4024 && device.fault == Fault::GxOff && (value & 1)) { value |= 0x80000000u; }
            } else {
                if (offset == 0x4028 && (value & 1) && (value & 0x6000) != 0x6000) {
                    device.violation = true; return false;
                }
                value = (value & 1) ? value & ~0x80000000u : value | 0x80000000u;
                if (device.fault == Fault::BranchOn && offset == 0x403c) { value |= 0x80000000u; }
                if (device.fault == Fault::Retention && offset == 0x4028) { value &= ~0x6000u; }
            }
            device.mmss[offset / 4] = value;
            return true;
        }
        if (!device.powered()) { device.violation = true; return false; }
        if (space == RegisterSpace::Gpu) {
            device.gpu[offset / 4] = value;
            if (offset == 0x98 * 4) {
                if (value == 0x778000 && device.fault != Fault::SpTpOn) { device.gpu[0x1b0] = 1u << 20; }
                if (value == 0x778001 && device.fault != Fault::SpTpOff) { device.gpu[0x1b0] = 0; }
            }
            if (offset == 0x227 * 4) { ++device.meWords; }
            if (offset == 0x224 * 4) { ++device.pfpWords; }
            if (offset == 0x205 * 4 && value) { device.execute(value); }
            if (offset == 0x22d * 4 && (value & (1u << 28)) && device.submissions && device.fault == Fault::HaltBusy) {
                device.gpu[0x191] = 0x80000000u;
            }
            return true;
        }
        if ((offset == 0x8000 || offset == 0x9000) && value == 0 &&
            device.batch.verifiedPasses == 2 && device.fault == Fault::DisableContext) { return true; }
        device.smmu[offset / 4] = value;
        if ((offset & ~0x1fu) == 0x8800 || (offset & ~0x1fu) == 0x9800) {
            if (offset & 4) { return true; }
            const u32 bank = offset < 0x9000 ? 0 : 1;
            ++device.translations[bank];
            const bool writing = (offset & 8) != 0;
            const u64 physical = device.translated(bank, value, writing);
            const bool invalid = device.fault == Fault::AtsFault ||
                (device.fault == Fault::SecondContextAts && bank == 1) ||
                (device.fault == Fault::AtsWriteFault && writing);
            const u32 output = invalid || !physical ? 1 : static_cast<u32>(physical & ~4095ull);
            device.smmu[(0x8050 + bank * 0x1000) / 4] = output ^ (device.fault == Fault::AtsWrongPhysical ? 4096u : 0u);
            device.smmu[(0x8054 + bank * 0x1000) / 4] = 0;
        }
        return true;
    }

    static void barrier(void *) {}
    static void delay(void *, u32) {}
    static bool cache(void *context, const DmaRegion &region, CacheOperation operation)
    {
        auto &device = *static_cast<Device *>(context);
        u32 *shadow = region.cpu == device.ring.data() ? device.deviceRing.data() :
            region.cpu == device.fence.data() ? device.deviceFence.data() :
            region.cpu == device.destination.data() ? device.deviceDestination.data() : nullptr;
        if (!shadow) { device.violation = true; return false; }
        if (operation == CacheOperation::Clean) {
            ++device.cacheCleans;
            std::memcpy(shadow, region.cpu, region.bytes);
        } else {
            ++device.cacheInvalidates;
            std::memcpy(region.cpu, shadow, region.bytes);
        }
        return true;
    }
    static bool cleanTables(void *context, const lpae::Tables &tables)
    {
        auto &device = *static_cast<Device *>(context);
        ++device.tableCleans;
        if (tables.rootPhysical != 0x88000000 || tables.poolPhysical != 0x88001000) { return false; }
        device.deviceRoot = device.root;
        device.devicePool = device.pool;
        return true;
    }

    u32 *shared(u64 address)
    {
        if (address >= 0x100000 && address < 0x100058) { return &control[static_cast<std::size_t>((address - 0x100000) / 4)]; }
        if (address >= 0x100200 && address < 0x100a00) { return &fifo[static_cast<std::size_t>((address - 0x100200) / 4)]; }
        return nullptr;
    }

    void remote()
    {
        if (fault == Fault::PeerSilent) { return; }
        if (control[0] == 1) { control[11] = 1; control[12] = 0; control[18] = 1; return; }
        if (control[0] == 4 || control[0] == 0) {
            if (fault == Fault::CloseSilent) { return; }
            control[11] = 4; control[12] = 1; control[18] = 1;
            closed = control[0] == 0;
            return;
        }
        control[11] = 2; control[12] = 1; control[18] = 1;
        if (control[9] == control[10]) { return; }
        u32 cursor = control[9];
        const u32 size = fifo[cursor / 4];
        if (size < 40 || size > 76 || (size & 3)) { violation = true; return; }
        for (u32 index = 1; index < 5; ++index) {
            if (fifo[((cursor + index * 4) & 1023) / 4]) { violation = true; return; }
        }
        std::array<u32, 19> words = {};
        for (u32 index = 0; index < size / 4; ++index) {
            words[index] = fifo[((cursor + 20 + index * 4) & 1023) / 4];
        }
        if (words[0] != 0x00716572 || words[1] != size - 8 || words[3] != 0 || words[6] != size - 28) {
            violation = true; return;
        }
        u32 resource = 4;
        for (u32 index = 0; index < plan.resourceCount; ++index) {
            if (words[4] == plan.resources[index].resource.type && words[5] == plan.resources[index].resource.id) { resource = index; }
        }
        if (resource == 4 || words[7] != 0x007a484b || words[8] != 4 || size != 40) { violation = true; return; }
        if (words[9] < votes[resource] && !quiescent()) { releaseBeforeQuiescence = true; }
        votes[resource] = words[9];
        requestIds.push_back(words[2]);
        requestedResources.push_back(resource);
        requestedValues.push_back(words[9]);
        control[9] = (cursor + size + 20) & 1023;
        control[17] = 1;
        if (fault == Fault::AckLost || (fault == Fault::ReleaseAckLost && requestIds.size() > 4)) { return; }
        const u32 ackId = words[2] + (fault == Fault::AckStale ? 1u : 0u);
        const u32 response[] = {0x00716572, 12, 0x2367736d, 4, ackId};
        cursor = control[21];
        fifo[256 + cursor / 4] = 20;
        for (u32 index = 1; index < 5; ++index) { fifo[256 + ((cursor + index * 4) & 1023) / 4] = 0; }
        for (u32 index = 0; index < 5; ++index) {
            fifo[256 + ((cursor + 20 + index * 4) & 1023) / 4] = response[index];
        }
        if (fault == Fault::AckMalformed) { fifo[256 + ((cursor + 20) & 1023) / 4] = 0; }
        if (fault == Fault::AckError) {
            fifo[256 + cursor / 4] = 32;
            fifo[256 + ((cursor + 24) & 1023) / 4] = 24;
            fifo[256 + ((cursor + 40) & 1023) / 4] = 0x00727265;
            fifo[256 + ((cursor + 44) & 1023) / 4] = 4;
            fifo[256 + ((cursor + 48) & 1023) / 4] = 0x00646162;
        }
        control[21] = (cursor + 20 + (fault == Fault::AckError ? 32 : 20)) & 1023;
        control[16] = 1;
    }

    static bool translate(void *context, u64 address, bool, u64 *par)
    {
        auto &device = *static_cast<Device *>(context);
        if (device.fault == Fault::SmemTranslation || (device.closed && device.fault == Fault::Retirement)) { return false; }
        for (const rpm::MemoryRegion &region : {device.plan.memory.control, device.plan.memory.fifo, device.plan.memory.doorbell}) {
            if (address >= region.address && address < region.address + region.bytes) {
                *par = (region.physical + address - region.address) & ~4095ull;
                return true;
            }
        }
        return false;
    }
    static bool memoryRead(void *context, u64 address, u32 *value)
    {
        auto &device = *static_cast<Device *>(context);
        u32 *word = device.shared(address);
        if (!word || !value) { return false; }
        *value = *word;
        return true;
    }
    static bool memoryWrite(void *context, u64 address, u32 value)
    {
        auto &device = *static_cast<Device *>(context);
        ++device.memoryWrites;
        if (address == 0x200008) {
            if (value != 1) { device.violation = true; return false; }
            device.remote();
            return true;
        }
        u32 *word = device.shared(address);
        if (!word) { return false; }
        *word = value;
        return true;
    }
    static bool memoryBarrier(void *) { return true; }
    BatchIo io()
    {
        return {{this, read, write, barrier, delay},
            {this, translate, memoryRead, memoryWrite, memoryBarrier, delay, true}, cleanTables, cache};
    }
    BatchResult run() { return runGpuBatch(batch, io(), buffers, plan, budget); }
};

void testPinnedPrerequisites()
{
    for (u32 mutation = 0; mutation < 8; ++mutation) {
        auto device = std::make_unique<Device>();
        power::Controller controller = {};
        power::PinnedDependencies dependencies = {980000, 300000, true, true, true, true};
        if (mutation == 0) { dependencies.observedRailReady = false; }
        if (mutation == 1) { dependencies.clockAcknowledged = false; }
        if (mutation == 2) { dependencies.inheritedBusesUntouched = false; }
        if (mutation == 3) { dependencies.exclusiveHardware = false; }
        if (mutation == 4) { dependencies.graphicsKHz = 19200; }
        if (mutation == 5) { dependencies.graphicsKHz = 600000; }
        if (mutation == 6) { dependencies.graphicsMicrovolts = 975000; }
        if (mutation == 7) { dependencies.graphicsMicrovolts = 1360000; }
        expect(power::startPinned(controller, device->io().registers, dependencies, 16) ==
            power::Result::Unqualified && !device->writes && !device->gpuReads && !device->smmuReads,
            "the pinned path rejects a stale rail wake clock other rate or unqualified supply before MMIO writes");
    }
    auto device = std::make_unique<Device>();
    device->votes = {2, 50000, 100000, 300000};
    power::Controller controller = {};
    const power::PinnedDependencies dependencies = {980000, 300000, true, true, true, true};
    expect(power::startPinned(controller, device->io().registers, dependencies, 16) == power::Result::Ok &&
        controller.state == power::State::Ready && device->powered() && !device->violation &&
        !device->gpuReads && !device->smmuReads && !device->memoryWrites,
        "qualified observed power and the exact startup rate enable domains and branches without GPU or DMA access");
    const u32 writes = device->writes;
    expect(power::startPinned(controller, device->io().registers, dependencies, 16) == power::Result::NotReady &&
        device->writes == writes, "the native pinned power sequence cannot be replayed");
    auto managed = std::make_unique<Device>();
    managed->votes = {2, 50000, 100000, 300000};
    power::Controller managedController = {};
    power::PinnedDependencies managedDependencies = {795000, 300000, true, true, true, true, true};
    expect(power::startPinned(managedController, managed->io().registers, managedDependencies, 16) ==
        power::Result::Ok && managed->powered() && !managed->gpuReads && !managed->smmuReads &&
        !managed->memoryWrites && !managed->violation,
        "OEM-managed voltage qualification is distinct from a fixed-980mV direct supply contract");
    for (u32 mutation = 0; mutation < 4; ++mutation) {
        auto refused = std::make_unique<Device>();
        power::Controller refusedController = {};
        power::PinnedDependencies unknown = managedDependencies;
        if (mutation == 0) { unknown.clockAcknowledged = false; }
        if (mutation == 1) { unknown.observedRailReady = false; }
        if (mutation == 2) { unknown.graphicsMicrovolts = 345000; }
        if (mutation == 3) { unknown.graphicsMicrovolts = 1360000; }
        expect(power::startPinned(refusedController, refused->io().registers, unknown, 16) ==
            power::Result::Unqualified && !refused->writes,
            "managed policy does not bypass clock ACK regulator readiness or valid hardware encoding");
    }
}

void testSuccess()
{
    auto device = std::make_unique<Device>();
    expect(device->run() == BatchResult::Ok, "full native-adapter/RPM/power/SMMU/CP batch completes in the independent device model");
    if (device->batch.stage != BatchStage::Complete) {
        std::printf("stage=%s result=%u rpm=%u power=%u hw=%u route=%u context=%u queue=%u\n",
            batchStageName(device->batch.failureStage), static_cast<unsigned>(device->batch.result),
            static_cast<unsigned>(device->batch.rpmResult), static_cast<unsigned>(device->batch.powerResult),
            static_cast<unsigned>(device->batch.hardwareResult), static_cast<unsigned>(device->batch.routeResult),
            static_cast<unsigned>(device->batch.contextResult), static_cast<unsigned>(device->batch.queueResult));
    }
    expect(!device->violation && !device->releaseBeforeQuiescence, "no premature MMIO, invalid PM4, foreign DMA or early resource release");
    expect(device->batch.verifiedPasses == 2 && device->fills == 2 && device->submissions == 3,
        "ME initialization plus two complementary poisoned-target memory-write submissions are verified");
    expect(device->batch.dmaReusable && !device->batch.resourcesPinned && device->quiescent() && device->closed,
        "DMA becomes reusable only after CP halt, both banks disabled, SP/TP off, domains off, votes restored and channel retired");
    expect(device->mmss[0x4028 / 4] == 0x80000ff0,
        "GX core-clock FF/memory-retention bits are asserted before enabling and restored after verified collapse");
    expect(device->batch.memory.state == rpm::MemoryState::Retired && device->batch.pipe.state == rpm::PipeState::Closed,
        "real native memory and SMD state machines retire the channel");
    const std::vector<u32> resources = {0, 1, 2, 3, 3, 2, 1, 0};
    const std::vector<u32> values = {2, 50000, 100000, 27000, 0, 19200, 19200, 1};
    expect(device->requestedResources == resources && device->requestedValues == values,
        "independent RPM decoder observes dependency order and reverse restoration to nonzero inherited baselines");
    for (u32 index = 0; index < device->requestIds.size(); ++index) {
        expect(device->requestIds[index] == 0x418000 + index, "every SMD-carried RPM request has a unique qualified-epoch message ID");
    }
    expect(device->translations[0] == 20 && device->translations[1] == 20,
        "both contexts translate all ring pages for reads and both fence/destination pages for reads and writes");
    expect(device->gpu[0x302c] == 0x18181818 && device->gpu[0x3049] == 3 && device->gpu[0x22e] == 0x02004000,
        "A418 uses A430-derived VBIF values and bit-14 CP bootstrap workaround, not A420 bit 29");
    expect(device->gpu[0x240] == 0x62000010 && device->gpu[0x24a] == 0x61003980 && device->gpu[0x24e] == 0x6e010000,
        "independent protected-range golden words cover privileged registers, XPU-sensitive blocks and SMMU");
    expect(device->gpu[0x250] == 7 && device->gpu[0x37] == 0 && device->meWords == 3 && device->pfpWords == 5,
        "protection stays enabled, firmware headers are skipped and polling does not enable GPU interrupts");
    expect(device->cacheInvalidates >= 9 && device->cacheCleans >= 8 && device->tableCleans == 2,
        "independent CPU/device views require actual clean and invalidate callbacks for tables, commands, destination and fences");
    for (u32 index = 0; index < 256; ++index) {
        expect(device->destination[index] == ~device->plan.fillValue, "every target word matches the second GPU-model write");
    }
    expect(device->run() == BatchResult::NotReady, "completed batch cannot reuse an epoch, channel binding or hardware ownership");
}

void testQualification()
{
    for (u32 index = 0; index < 15; ++index) {
        auto device = std::make_unique<Device>();
        bool *claims[] = {&device->plan.firmwareExited, &device->plan.exclusiveChannel,
            &device->plan.messageEpochQualified, &device->plan.baselineQualified,
            &device->plan.dmaOwnershipQualified, &device->plan.firmwareDigestsQualified,
            &device->plan.power.boardProfileQualified, &device->plan.power.exclusiveHardware,
            &device->plan.hardware.identityQualified, &device->plan.hardware.exclusiveOwnership,
            &device->plan.routes.platformQualified, &device->plan.routes.exclusiveOwnership,
            &device->plan.routes.nonSecureEl1};
        if (index < 13) { *claims[index] = false; }
        else if (index == 13) { device->plan.hardware.chipId = 0x04020000; }
        else { device->plan.firstMessageId = 0xffffffffu; }
        expect(device->run() != BatchResult::Ok && device->batch.failureStage == BatchStage::Qualifying,
            "every missing platform/ownership/baseline/epoch/firmware qualification refuses the batch");
        expect(device->writes == 0 && device->memoryWrites == 0 && device->gpuReads == 0 && device->smmuReads == 0,
            "unqualified inputs cause no RPM publication or hardware accesses");
    }
    for (u32 index = 0; index < 13; ++index) {
        auto device = std::make_unique<Device>();
        if (index == 0) { device->buffers.destination.physical = device->buffers.ring.physical; }
        if (index == 1) { device->buffers.destination.gpu = device->buffers.ring.gpu; }
        if (index == 2) { device->buffers.destination.cpu = device->ring.data(); }
        if (index == 3) { device->buffers.destination.physical = device->buffers.tables.rootPhysical; }
        if (index == 4) { device->buffers.destination.cpu = reinterpret_cast<u32 *>(device->root.data()); }
        if (index == 5) { device->buffers.tables.root[0] = 3; }
        if (index == 6) { device->buffers.tables.usedPages = 1; }
        if (index == 7) { device->buffers.tables.poolPages = 0xffffffffu; }
        if (index == 8) { device->plan.fillDwords = 257; }
        if (index == 9) { device->budget.rpmPolls = 1; }
        if (index == 10) { device->plan.resourceCount = 9; }
        if (index == 11) { device->plan.resources[0].maximumAggregationQualified = false; }
        if (index == 12) { device->plan.power.votes[3].values[0].value = 600001; }
        expect(device->run() == BatchResult::Invalid && device->writes == 0 && device->memoryWrites == 0,
            "invalid budgets, allocations, firmware, votes and rate limits fail before MMIO or transport publication");
    }
}

void testFailures()
{
    struct Case { Fault fault; BatchStage stage; };
    const Case cases[] = {
        {Fault::SmemTranslation, BatchStage::Observing}, {Fault::PeerSilent, BatchStage::Opening},
        {Fault::AckLost, BatchStage::Powering}, {Fault::AckMalformed, BatchStage::Powering},
        {Fault::AckError, BatchStage::Powering}, {Fault::AckStale, BatchStage::Powering},
        {Fault::GxOn, BatchStage::Powering}, {Fault::BranchOn, BatchStage::Powering},
        {Fault::Retention, BatchStage::Powering},
        {Fault::SpTpOn, BatchStage::Initializing}, {Fault::Protection, BatchStage::Initializing},
        {Fault::SmmuGlobalAbort, BatchStage::Routing},
        {Fault::RouteBypass, BatchStage::Routing}, {Fault::RouteAlias, BatchStage::Routing},
        {Fault::ContextEnabled, BatchStage::Routing}, {Fault::ContextReadback, BatchStage::Binding},
        {Fault::TlbBusy, BatchStage::Binding}, {Fault::AtsFault, BatchStage::Translating},
        {Fault::AtsWrongPhysical, BatchStage::Translating}, {Fault::SecondContextAts, BatchStage::Translating},
        {Fault::AtsWriteFault, BatchStage::Translating},
        {Fault::CpStall, BatchStage::WaitingInit}, {Fault::BadFence, BatchStage::WaitingInit},
        {Fault::CorruptFill, BatchStage::Verifying}, {Fault::DestinationGuard, BatchStage::Verifying},
        {Fault::FenceGuard, BatchStage::WaitingInit}, {Fault::HaltBusy, BatchStage::Halting},
        {Fault::DisableContext, BatchStage::Disabling}, {Fault::SpTpOff, BatchStage::StoppingHardware},
        {Fault::GxOff, BatchStage::StoppingPower}, {Fault::ReleaseAckLost, BatchStage::StoppingPower},
        {Fault::CloseSilent, BatchStage::Closing}, {Fault::Retirement, BatchStage::Retiring}
    };
    for (const Case &test : cases) {
        auto device = std::make_unique<Device>();
        device->fault = test.fault;
        const BatchResult result = device->run();
        if (result == BatchResult::Ok || device->batch.failureStage != test.stage) {
            std::printf("fault=%u expected=%s actual=%s result=%u\n", static_cast<unsigned>(test.fault),
                batchStageName(test.stage), batchStageName(device->batch.failureStage), static_cast<unsigned>(result));
        }
        expect(result != BatchResult::Ok && device->batch.stage == BatchStage::Failed && device->batch.failureStage == test.stage,
            "injected fault stops at its actual pipeline boundary with diagnostic state retained");
        expect(!device->batch.dmaReusable && !device->releaseBeforeQuiescence && !device->violation,
            "failure never releases uncertain DMA or sends destructive cleanup votes");
        if (test.stage != BatchStage::Observing) {
            expect(device->batch.resourcesPinned, "after channel claim, failures pin resources instead of pretending teardown succeeded");
        }
        if (test.stage <= BatchStage::Powering) {
            expect(device->gpuReads == 0 && device->smmuReads == 0 && device->submissions == 0,
                "missing ACKs, domains or clocks never lead to GPU/SMMU accesses");
        }
        if (test.stage <= BatchStage::Translating) {
            expect(device->submissions == 0 && device->meWords == 0 && device->pfpWords == 0,
                "unqualified stream routes or failed translations never start microcode or DMA");
        }
        if (test.fault == Fault::SmmuGlobalAbort) {
            expect(device->batch.hardware.state == HardwareState::Ready &&
                device->batch.routeResult == smmu::RouteResult::IoError && !device->batch.routes.qualified,
                "handset-style global ID read abort preserves hardware success but never qualifies GPU DMA");
        }
        if (test.fault == Fault::AckLost || test.fault == Fault::AckMalformed || test.fault == Fault::AckStale) {
            expect(device->requestIds.size() == 1 && device->writes == 0 && device->batch.votes.state == rpm::VoteState::Poisoned,
                "uncertain first resource ACK is never retried, rolled back or followed by a domain write");
        }
    }
    auto reference = std::make_unique<Device>();
    expect(reference->run() == BatchResult::Ok, "write-fault sweep obtains a successful reference trace");
    const u32 totalWrites = reference->writes;
    for (u32 index = 1; index <= totalWrites; ++index) {
        auto device = std::make_unique<Device>();
        device->failWrite = index;
        expect(device->run() != BatchResult::Ok && device->batch.stage == BatchStage::Failed &&
            device->batch.resourcesPinned && !device->batch.dmaReusable,
            "every individual MMIO write failure holds ownership and refuses false completion");
        expect(!device->violation && !device->releaseBeforeQuiescence,
            "no injected write failure causes premature accesses or unsafe rollback");
    }
}

}

int main()
{
    testPinnedPrerequisites();
    testSuccess();
    testQualification();
    testFailures();
    std::printf("integrated GPU batch model: %u checks, %u failures (not a handset GPU result)\n", checks, failures);
    return failures ? 1 : 0;
}
