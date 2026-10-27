#include "GpuBatch.h"
#include "BoardConstants.h"

namespace adreno {
namespace {

constexpr u32 kCanary = 0x418c0de5;

bool overlaps(u64 first, u64 firstBytes, u64 second, u64 secondBytes)
{
    return first < second + secondBytes && second < first + firstBytes;
}

bool validRegion(const DmaRegion &region, u32 bytes)
{
    return region.cpu && (reinterpret_cast<u64>(region.cpu) & 4095) == 0 &&
        region.bytes == bytes && (region.physical & 4095) == 0 && (region.gpu & 4095) == 0 &&
        region.physical < (1ull << 32) && bytes <= (1ull << 32) - region.physical &&
        bytes <= (1ull << 32) - region.gpu && reinterpret_cast<u64>(region.cpu) <= ~0ull - bytes;
}

bool validBuffers(const BatchBuffers &buffers)
{
    const lpae::Tables &tables = buffers.tables;
    pm4::UcodePlan pm4 = {};
    pm4::UcodePlan pfp = {};
    if (!lpae::validTables(tables) || tables.usedPages || tables.poolPages < 6 || tables.poolPages > 64 ||
        (reinterpret_cast<u64>(tables.root) & 4095) || (reinterpret_cast<u64>(tables.pool) & 4095) ||
        !validRegion(buffers.ring, lumia950::kCpRingBytes) || !validRegion(buffers.fence, 4096) ||
        !validRegion(buffers.destination, 4096) ||
        !pm4::ringBaseUsable(buffers.ring.gpu, buffers.ring.bytes) ||
        !pm4::planUcodeLoad(buffers.pm4, buffers.pm4Words, pm4::CpUcode::Pm4, &pm4) ||
        !pm4::planUcodeLoad(buffers.pfp, buffers.pfpWords, pm4::CpUcode::Pfp, &pfp)) { return false; }
    for (u32 index = 0; index < lpae::kEntries; ++index) {
        if (tables.root[index]) { return false; }
    }
    const DmaRegion regions[] = {buffers.ring, buffers.fence, buffers.destination};
    const u64 tableCpu[] = {reinterpret_cast<u64>(tables.root), reinterpret_cast<u64>(tables.pool)};
    const u64 tablePhysical[] = {tables.rootPhysical, tables.poolPhysical};
    const u64 tableBytes[] = {4096, static_cast<u64>(tables.poolPages) * 4096};
    const u64 firmware[] = {reinterpret_cast<u64>(buffers.pm4), reinterpret_cast<u64>(buffers.pfp)};
    const u64 firmwareBytes[] = {static_cast<u64>(buffers.pm4Words) * 4, static_cast<u64>(buffers.pfpWords) * 4};
    for (u32 first = 0; first < 3; ++first) {
        const DmaRegion &region = regions[first];
        for (u32 second = 0; second < first; ++second) {
            const DmaRegion &previous = regions[second];
            if (overlaps(region.physical, region.bytes, previous.physical, previous.bytes) ||
                overlaps(region.gpu, region.bytes, previous.gpu, previous.bytes) ||
                overlaps(reinterpret_cast<u64>(region.cpu), region.bytes,
                         reinterpret_cast<u64>(previous.cpu), previous.bytes)) { return false; }
        }
        for (u32 index = 0; index < 2; ++index) {
            if (overlaps(region.physical, region.bytes, tablePhysical[index], tableBytes[index]) ||
                overlaps(reinterpret_cast<u64>(region.cpu), region.bytes, tableCpu[index], tableBytes[index]) ||
                firmware[index] > ~0ull - firmwareBytes[index] ||
                overlaps(reinterpret_cast<u64>(region.cpu), region.bytes, firmware[index], firmwareBytes[index])) {
                return false;
            }
        }
    }
    for (u32 first = 0; first < 2; ++first) {
        for (u32 second = 0; second < 2; ++second) {
            if (overlaps(tableCpu[first], tableBytes[first], firmware[second], firmwareBytes[second])) {
                return false;
            }
        }
    }
    return true;
}

BatchResult fail(GpuBatch &batch, BatchResult result)
{
    batch.failureStage = batch.stage;
    batch.stage = BatchStage::Failed;
    batch.result = result;
    batch.dmaReusable = false;
    return result;
}

bool read(void *context, RegisterSpace space, u32 offset, u32 *value)
{
    auto &batch = *static_cast<GpuBatch *>(context);
    return batch.io.registers.read(batch.io.registers.context, space, offset, value);
}

bool write(void *context, RegisterSpace space, u32 offset, u32 value)
{
    auto &batch = *static_cast<GpuBatch *>(context);
    return batch.io.registers.write(batch.io.registers.context, space, offset, value);
}

void barrier(void *context)
{
    auto &batch = *static_cast<GpuBatch *>(context);
    batch.io.registers.barrier(batch.io.registers.context);
}

void delay(void *context, u32 microseconds)
{
    auto &batch = *static_cast<GpuBatch *>(context);
    batch.io.registers.delayUs(batch.io.registers.context, microseconds);
}

bool mapped(void *context, const DmaRegion &region, bool writable)
{
    auto &batch = *static_cast<GpuBatch *>(context);
    return batch.contexts[0].enabled && batch.contexts[1].enabled &&
        lpae::matches(batch.buffers.tables, region.gpu, region.physical, region.bytes, writable);
}

bool cache(void *context, const DmaRegion &region, CacheOperation operation)
{
    auto &batch = *static_cast<GpuBatch *>(context);
    return batch.io.cache(batch.io.registers.context, region, operation);
}

bool guards(GpuBatch &batch, u32 fillDwords)
{
    const DmaRegion regions[] = {batch.buffers.destination, batch.buffers.fence};
    for (u32 region = 0; region < 2; ++region) {
        if (!batch.io.cache(batch.io.registers.context, regions[region], CacheOperation::Invalidate)) { return false; }
        batch.io.registers.barrier(batch.io.registers.context);
        const volatile u32 *words = regions[region].cpu;
        for (u32 index = region == 0 ? fillDwords : 1; index < regions[region].bytes / 4; ++index) {
            if (words[index] != (kCanary ^ index)) { return false; }
        }
    }
    return *static_cast<volatile u32 *>(batch.buffers.fence.cpu) == batch.queue.sequence;
}

}

BatchResult runGpuBatch(GpuBatch &batch, const BatchIo &io, const BatchBuffers &buffers,
    const BatchPlan &plan, const BatchBudget &budget)
{
    if (batch.stage != BatchStage::Empty) { return BatchResult::NotReady; }
    batch.stage = BatchStage::Qualifying;
    if (!plan.firmwareExited || !plan.exclusiveChannel || !plan.messageEpochQualified ||
        !plan.baselineQualified || !plan.dmaOwnershipQualified || !plan.firmwareDigestsQualified ||
        !plan.power.boardProfileQualified || !plan.power.exclusiveHardware ||
        !validHardwareContract(plan.hardware) || !plan.routes.platformQualified ||
        !plan.routes.exclusiveOwnership || !plan.routes.nonSecureEl1 || !io.memory.nonSecureEl1) {
        return fail(batch, BatchResult::Unqualified);
    }
    if (!io.registers.read || !io.registers.write || !io.registers.barrier || !io.registers.delayUs ||
        !io.cleanTables || !io.cache || !plan.firstMessageId ||
        plan.firstMessageId > 0xffffffffu - 2 * rpm::kVoteResources ||
        !plan.fillDwords || plan.fillDwords > 256 ||
        budget.rpmPolls < 2 || budget.rpmPolls > 100000 || budget.rpmDelayUs > 1000 ||
        static_cast<u64>(budget.rpmPolls) * budget.rpmDelayUs > 5000000 ||
        !budget.mmioPolls || budget.mmioPolls > 100000 || !budget.gpuPolls || budget.gpuPolls > 100000 ||
        !validBuffers(buffers)) { return fail(batch, BatchResult::Invalid); }
    batch.io = io;
    batch.buffers = buffers;
    batch.rpmResult = rpm::bindSession(batch.session, rpm::pipeTransport(batch.pipe), plan.firstMessageId, true);
    if (batch.rpmResult != rpm::Result::Ok ||
        rpm::bindVotes(batch.votes, batch.session, plan.resources, plan.resourceCount, true, true) != rpm::VoteResult::Ok ||
        !power::validPlan(plan.power, batch.votes)) { return fail(batch, BatchResult::Invalid); }
    batch.stage = BatchStage::Mapping;
    const DmaRegion regions[] = {buffers.ring, buffers.fence, buffers.destination};
    for (u32 index = 0; index < 3; ++index) {
        if (!lpae::mapRange(batch.buffers.tables, regions[index].gpu, regions[index].physical,
                            regions[index].bytes, index != 0, false)) { return fail(batch, BatchResult::MappingFailed); }
    }
    for (u32 region = 1; region < 3; ++region) {
        for (u32 index = 0; index < regions[region].bytes / 4; ++index) {
            regions[region].cpu[index] = kCanary ^ index;
        }
        if (!io.cache(io.registers.context, regions[region], CacheOperation::Clean)) {
            return fail(batch, BatchResult::MappingFailed);
        }
    }
    batch.stage = BatchStage::Observing;
    batch.rpmResult = rpm::observeMemory(batch.memory, plan.memory, io.memory);
    if (batch.rpmResult != rpm::Result::Ok) { return fail(batch, BatchResult::MemoryFailed); }
    batch.stage = BatchStage::Claiming;
    batch.rpmResult = rpm::claimMemory(batch.memory, true, true);
    if (batch.rpmResult != rpm::Result::Ok) { return fail(batch, BatchResult::MemoryFailed); }
    batch.resourcesPinned = true;
    batch.stage = BatchStage::Opening;
    batch.rpmResult = rpm::openPipe(batch.pipe, rpm::memoryIo(batch.memory), 88, plan.memory.fifo.bytes / 2,
        true, true, budget.rpmPolls, budget.rpmDelayUs);
    if (batch.rpmResult != rpm::Result::Ok) { return fail(batch, BatchResult::ChannelFailed); }
    batch.stage = BatchStage::Powering;
    batch.powerResult = power::start(batch.power, io.registers, batch.votes, plan.power,
        budget.rpmPolls, budget.rpmDelayUs, budget.mmioPolls);
    if (batch.powerResult != power::Result::Ok) { return fail(batch, BatchResult::PowerFailed); }
    batch.stage = BatchStage::Initializing;
    batch.hardwareResult = initializeCommandHardware(batch.hardware, io.registers, plan.hardware, budget.mmioPolls);
    if (batch.hardwareResult != HardwareResult::Ok) { return fail(batch, BatchResult::HardwareFailed); }
    batch.stage = BatchStage::Routing;
    batch.routeResult = smmu::inspectRoutes(batch.routes, io.registers, plan.routes);
    if (batch.routeResult != smmu::RouteResult::Ok) { return fail(batch, BatchResult::RouteFailed); }
    batch.stage = BatchStage::Binding;
    for (u32 index = 0; index < 2; ++index) {
        batch.contextResult = bindDisabledContext(batch.contexts[index], {io.registers, io.cleanTables},
            batch.buffers.tables, index, index + 1, true, budget.mmioPolls);
        if (batch.contextResult != ContextResult::Ok) { return fail(batch, BatchResult::ContextFailed); }
    }
    batch.stage = BatchStage::Translating;
    batch.routeResult = smmu::revalidateRoutes(batch.routes, true);
    if (batch.routeResult != smmu::RouteResult::Ok) { return fail(batch, BatchResult::RouteFailed); }
    for (u32 bankIndex = 0; bankIndex < 2; ++bankIndex) {
        ContextBank &bank = batch.contexts[bankIndex];
        const ContextAccess readAccess = bankIndex == 0 ? ContextAccess::UnprivilegedRead : ContextAccess::PrivilegedRead;
        const ContextAccess writeAccess = bankIndex == 0 ? ContextAccess::UnprivilegedWrite : ContextAccess::PrivilegedWrite;
        for (u32 regionIndex = 0; regionIndex < 3; ++regionIndex) {
            const DmaRegion &region = regions[regionIndex];
            for (u32 offset = 0; offset < region.bytes; offset += 4096) {
                batch.contextResult = checkTranslationAccess(bank, region.gpu + offset,
                    static_cast<u32>(region.physical + offset), readAccess, budget.mmioPolls);
                if (batch.contextResult != ContextResult::Ok) { return fail(batch, BatchResult::ContextFailed); }
                if (regionIndex != 0) {
                    batch.contextResult = checkTranslationAccess(bank, region.gpu + offset,
                        static_cast<u32>(region.physical + offset), writeAccess, budget.mmioPolls);
                    if (batch.contextResult != ContextResult::Ok) { return fail(batch, BatchResult::ContextFailed); }
                }
            }
        }
    }
    batch.stage = BatchStage::StartingQueue;
    const SubmissionIo submission = {{&batch, read, write, barrier, delay}, mapped, cache};
    batch.queueResult = initializeQueue(batch.queue, submission, buffers.ring, buffers.fence,
        buffers.pm4, buffers.pm4Words, buffers.pfp, buffers.pfpWords, true);
    if (batch.queueResult != QueueResult::Ok) { return fail(batch, BatchResult::QueueFailed); }
    batch.stage = BatchStage::WaitingInit;
    batch.queueResult = waitQueue(batch.queue, budget.gpuPolls, 1);
    if (batch.queueResult != QueueResult::Ok) { return fail(batch, BatchResult::QueueFailed); }
    if (!guards(batch, 0)) { return fail(batch, BatchResult::ReadbackFailed); }
    for (u32 pass = 0; pass < 2; ++pass) {
        const u32 value = pass == 0 ? plan.fillValue : ~plan.fillValue;
        batch.stage = BatchStage::Filling;
        batch.queueResult = submitFill(batch.queue, buffers.destination, value, plan.fillDwords);
        if (batch.queueResult != QueueResult::Ok) { return fail(batch, BatchResult::QueueFailed); }
        batch.stage = BatchStage::WaitingFill;
        batch.queueResult = waitQueue(batch.queue, budget.gpuPolls, 1);
        if (batch.queueResult != QueueResult::Ok) { return fail(batch, BatchResult::QueueFailed); }
        batch.stage = BatchStage::Verifying;
        batch.queueResult = verifyFill(batch.queue, buffers.destination, value, plan.fillDwords);
        if (batch.queueResult != QueueResult::Ok || !guards(batch, plan.fillDwords)) {
            return fail(batch, BatchResult::ReadbackFailed);
        }
        ++batch.verifiedPasses;
    }
    batch.stage = BatchStage::Halting;
    batch.queueResult = haltQueue(batch.queue);
    if (batch.queueResult != QueueResult::Ok) { return fail(batch, BatchResult::QueueFailed); }
    batch.routeResult = smmu::revalidateRoutes(batch.routes, true);
    if (batch.routeResult != smmu::RouteResult::Ok) { return fail(batch, BatchResult::RouteFailed); }
    batch.stage = BatchStage::Disabling;
    for (ContextBank &bank : batch.contexts) {
        batch.contextResult = disableContext(bank, budget.mmioPolls);
        if (batch.contextResult != ContextResult::Ok) { return fail(batch, BatchResult::ContextFailed); }
    }
    batch.stage = BatchStage::StoppingHardware;
    batch.hardwareResult = stopCommandHardware(batch.hardware, true, budget.mmioPolls);
    if (batch.hardwareResult != HardwareResult::Ok) { return fail(batch, BatchResult::HardwareFailed); }
    batch.stage = BatchStage::StoppingPower;
    batch.powerResult = power::stop(batch.power, true, budget.rpmPolls, budget.rpmDelayUs, budget.mmioPolls);
    if (batch.powerResult != power::Result::Ok) { return fail(batch, BatchResult::PowerFailed); }
    batch.stage = BatchStage::Closing;
    batch.rpmResult = rpm::closePipe(batch.pipe, budget.rpmPolls, budget.rpmDelayUs);
    if (batch.rpmResult != rpm::Result::Ok) { return fail(batch, BatchResult::ChannelFailed); }
    batch.stage = BatchStage::Retiring;
    batch.rpmResult = rpm::retireMemory(batch.memory, batch.pipe);
    if (batch.rpmResult != rpm::Result::Ok) { return fail(batch, BatchResult::MemoryFailed); }
    batch.resourcesPinned = false;
    batch.dmaReusable = true;
    batch.stage = BatchStage::Complete;
    batch.result = BatchResult::Ok;
    return BatchResult::Ok;
}

const char *batchStageName(BatchStage stage)
{
    switch (stage) {
    case BatchStage::Empty: return "EMPTY";
    case BatchStage::Qualifying: return "QUALIFYING";
    case BatchStage::Mapping: return "MAPPING";
    case BatchStage::Observing: return "OBSERVING";
    case BatchStage::Claiming: return "CLAIMING";
    case BatchStage::Opening: return "OPENING";
    case BatchStage::Powering: return "POWERING";
    case BatchStage::Initializing: return "INITIALIZING";
    case BatchStage::Routing: return "ROUTING";
    case BatchStage::Binding: return "BINDING";
    case BatchStage::Translating: return "TRANSLATING";
    case BatchStage::StartingQueue: return "STARTING_QUEUE";
    case BatchStage::WaitingInit: return "WAITING_INIT";
    case BatchStage::Filling: return "FILLING";
    case BatchStage::WaitingFill: return "WAITING_FILL";
    case BatchStage::Verifying: return "VERIFYING";
    case BatchStage::Halting: return "HALTING";
    case BatchStage::Disabling: return "DISABLING";
    case BatchStage::StoppingHardware: return "STOPPING_HARDWARE";
    case BatchStage::StoppingPower: return "STOPPING_POWER";
    case BatchStage::Closing: return "CLOSING";
    case BatchStage::Retiring: return "RETIRING";
    case BatchStage::Complete: return "COMPLETE";
    case BatchStage::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

}
