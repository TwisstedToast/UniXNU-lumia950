#include "Submission.h"
#include "AdrenoCore.h"
#include "BoardConstants.h"

namespace adreno {
namespace {

bool validRegion(const DmaRegion &region)
{
    return region.cpu && (reinterpret_cast<u64>(region.cpu) & 4095) == 0 &&
        region.bytes != 0 && (region.bytes & 4095) == 0 &&
        (region.physical & 4095) == 0 && (region.gpu & 4095) == 0 &&
        region.physical < (1ull << 32) && region.bytes <= (1ull << 32) - region.physical &&
        region.bytes <= (1ull << 32) - region.gpu &&
        reinterpret_cast<u64>(region.cpu) <= ~0ull - region.bytes;
}

bool intervalsOverlap(u64 first, u32 firstBytes, u64 second, u32 secondBytes)
{
    return first < second + secondBytes && second < first + firstBytes;
}

bool overlaps(const DmaRegion &first, const DmaRegion &second)
{
    return intervalsOverlap(first.physical, first.bytes, second.physical, second.bytes) ||
        intervalsOverlap(first.gpu, first.bytes, second.gpu, second.bytes) ||
        intervalsOverlap(reinterpret_cast<u64>(first.cpu), first.bytes,
                         reinterpret_cast<u64>(second.cpu), second.bytes);
}

bool readGpu(Queue &queue, u32 index, u32 *value)
{
    return queue.io.registers.read(queue.io.registers.context, RegisterSpace::Gpu,
                                    index * 4, value);
}

bool writeGpu(Queue &queue, u32 index, u32 value)
{
    return queue.io.registers.write(queue.io.registers.context, RegisterSpace::Gpu,
                                     index * 4, value);
}

QueueResult fail(Queue &queue, QueueResult result)
{
    queue.state = QueueState::Failed;
    return result;
}

QueueResult ringSubmit(Queue &queue, const u32 *commands, u32 count)
{
    u32 readPointer = 0;
    if (!readGpu(queue, lumia950::kRegCpRbRptr, &readPointer)) {
        return fail(queue, QueueResult::IoError);
    }
    const u32 ringDwords = queue.ring.bytes / 4;
    if (readPointer >= ringDwords || queue.writePointer >= ringDwords) {
        return fail(queue, QueueResult::Invalid);
    }
    if (!pm4::ringHasRoom(queue.writePointer, readPointer, ringDwords, count)) {
        return QueueResult::NoRoom;
    }
    for (u32 index = 0; index < count; index++) {
        queue.ring.cpu[(queue.writePointer + index) & (ringDwords - 1)] = commands[index];
    }
    if (!queue.io.cache(queue.io.registers.context, queue.ring, CacheOperation::Clean)) {
        return fail(queue, QueueResult::IoError);
    }
    queue.io.registers.barrier(queue.io.registers.context);
    const u32 next = pm4::ringWritePointerAfter(queue.writePointer, count, ringDwords);
    queue.writePointer = next;
    if (!writeGpu(queue, lumia950::kRegCpRbWptr, next)) {
        return fail(queue, QueueResult::IoError);
    }
    queue.io.registers.barrier(queue.io.registers.context);
    queue.state = QueueState::Pending;
    return QueueResult::Ok;
}

bool loadMicrocode(Queue &queue, const pm4::UcodePlan &plan)
{
    if (!writeGpu(queue, plan.addressIndex, 0)) { return false; }
    for (u32 index = 0; index < plan.dataWords; index++) {
        if (!writeGpu(queue, plan.dataIndex, plan.data[index])) { return false; }
    }
    return true;
}

}

QueueResult initializeQueue(Queue &queue, const SubmissionIo &io,
    const DmaRegion &ring, const DmaRegion &fence,
    const u32 *pm4, u32 pm4Words, const u32 *pfp, u32 pfpWords,
    bool exclusiveOwnership)
{
    if (queue.state != QueueState::Empty || !exclusiveOwnership ||
        !io.registers.read || !io.registers.write || !io.registers.barrier ||
        !io.registers.delayUs || !io.mapped || !io.cache ||
        !validRegion(ring) || !validRegion(fence) || overlaps(ring, fence) ||
        !pm4::ringBaseUsable(ring.gpu, ring.bytes) || pm4::ringControl(ring.bytes) == 0) {
        return QueueResult::Invalid;
    }
    pm4::UcodePlan pm4Plan = {};
    pm4::UcodePlan pfpPlan = {};
    if (!pm4::planUcodeLoad(pm4, pm4Words, pm4::CpUcode::Pm4, &pm4Plan) ||
        !pm4::planUcodeLoad(pfp, pfpWords, pm4::CpUcode::Pfp, &pfpPlan)) {
        return QueueResult::Invalid;
    }
    const DmaRegion regions[] = {ring, fence};
    for (const DmaRegion &region : regions) {
        if (intervalsOverlap(reinterpret_cast<u64>(pm4), pm4Words * 4,
                             reinterpret_cast<u64>(region.cpu), region.bytes) ||
            intervalsOverlap(reinterpret_cast<u64>(pfp), pfpWords * 4,
                             reinterpret_cast<u64>(region.cpu), region.bytes)) {
            return QueueResult::Invalid;
        }
    }
    if (!io.mapped(io.registers.context, ring, false) ||
        !io.mapped(io.registers.context, fence, true)) { return QueueResult::MappingMissing; }
    GpuSnapshot snapshot = {};
    const ProbeResult ready = readSnapshot(io.registers, &snapshot);
    if (ready != ProbeResult::Ready) { return QueueResult::NotReady; }
    if ((snapshot.status & kGpuBusy) != 0 || snapshot.readPointer != 0 ||
        snapshot.writePointer != 0) {
        return QueueResult::Busy;
    }
    if ((snapshot.interruptStatus & kGpuFaultMask) != 0) { return QueueResult::GpuFault; }
    queue.io = io;
    queue.ring = ring;
    queue.fence = fence;
    queue.destination = {};
    queue.fillDwords = 0;
    queue.state = QueueState::Initializing;
    queue.ownsHardware = true;
    queue.writePointer = 0;
    queue.sequence = 1;
    queue.fault = 0;
    queue.polls = 0;
    if (!writeGpu(queue, lumia950::kRegCpMeCntl, kMeHalt)) {
        return fail(queue, QueueResult::IoError);
    }
    queue.io.registers.barrier(io.registers.context);
    for (u32 index = 0; index < ring.bytes / 4; index++) { ring.cpu[index] = 0; }
    fence.cpu[0] = 0;
    if (!io.cache(io.registers.context, ring, CacheOperation::Clean) ||
        !io.cache(io.registers.context, fence, CacheOperation::Clean) ||
        !loadMicrocode(queue, pm4Plan) || !loadMicrocode(queue, pfpPlan) ||
        !writeGpu(queue, lumia950::kRegCpRbCntl, pm4::ringControl(ring.bytes)) ||
        !writeGpu(queue, lumia950::kRegCpRbBase, ring.gpu) ||
        !writeGpu(queue, lumia950::kRegCpRbWptr, 0)) { return fail(queue, QueueResult::IoError); }
    u32 commands[26];
    const u32 initWords = pm4::writeMeInit(commands, 26);
    const u32 tailWords = pm4::writeSubmissionTail(commands + initWords, 26 - initWords,
                                                 fence.gpu, queue.sequence, false);
    queue.io.registers.barrier(io.registers.context);
    if (!writeGpu(queue, lumia950::kRegCpMeCntl, 0)) {
        return fail(queue, QueueResult::IoError);
    }
    return ringSubmit(queue, commands, initWords + tailWords);
}

QueueResult submitFill(Queue &queue, const DmaRegion &destination, u32 value, u32 dwords)
{
    if (queue.state != QueueState::Ready) { return QueueResult::NotReady; }
    if (!validRegion(destination) || dwords == 0 || dwords > 256 ||
        dwords > destination.bytes / 4 || overlaps(destination, queue.ring) ||
        overlaps(destination, queue.fence)) { return QueueResult::Invalid; }
    if (!queue.io.mapped(queue.io.registers.context, destination, true)) {
        return QueueResult::MappingMissing;
    }
    for (u32 index = 0; index < dwords; index++) { destination.cpu[index] = ~value; }
    if (!queue.io.cache(queue.io.registers.context, destination, CacheOperation::Clean)) {
        return fail(queue, QueueResult::IoError);
    }
    u32 commands[266];
    const u32 fillWords = pm4::writeMemoryFill(commands, 266, destination.gpu, value, dwords);
    const u32 nextSequence = queue.sequence + 1;
    const u32 tailWords = pm4::writeSubmissionTail(commands + fillWords, 266 - fillWords,
                                                 queue.fence.gpu, nextSequence, false);
    const QueueResult result = ringSubmit(queue, commands, fillWords + tailWords);
    if (result == QueueResult::Ok) {
        queue.sequence = nextSequence;
        queue.destination = destination;
        queue.fillValue = value;
        queue.fillDwords = dwords;
    }
    return result;
}

QueueResult waitQueue(Queue &queue, u32 pollBudget, u32 delayUs)
{
    if (queue.state != QueueState::Pending || pollBudget == 0 || pollBudget > 100000 ||
        delayUs > 1000) { return QueueResult::Invalid; }
    for (u32 index = 0; index < pollBudget; index++) {
        u32 interruptStatus = 0;
        if (!readGpu(queue, kInterruptStatusIndex, &interruptStatus) ||
            !queue.io.cache(queue.io.registers.context, queue.fence, CacheOperation::Invalidate)) {
            return fail(queue, QueueResult::IoError);
        }
        queue.polls++;
        if ((interruptStatus & kGpuFaultMask) != 0) {
            queue.fault = interruptStatus;
            return fail(queue, QueueResult::GpuFault);
        }
        queue.io.registers.barrier(queue.io.registers.context);
        const u32 sample = *static_cast<volatile u32 *>(queue.fence.cpu);
        if (pm4::fenceReached(sample, queue.sequence)) {
            u32 readPointer = 0;
            if (!readGpu(queue, lumia950::kRegCpRbRptr, &readPointer)) {
                return fail(queue, QueueResult::IoError);
            }
            if (readPointer == queue.writePointer) {
                queue.state = QueueState::Ready;
                return QueueResult::Ok;
            }
        }
        queue.io.registers.delayUs(queue.io.registers.context, delayUs);
    }
    return fail(queue, QueueResult::TimedOut);
}

QueueResult verifyFill(Queue &queue, const DmaRegion &destination, u32 value, u32 dwords)
{
    if (queue.state != QueueState::Ready) { return QueueResult::NotReady; }
    if (!validRegion(destination) || dwords == 0 || dwords != queue.fillDwords ||
        value != queue.fillValue || destination.cpu != queue.destination.cpu ||
        destination.physical != queue.destination.physical ||
        destination.gpu != queue.destination.gpu || destination.bytes != queue.destination.bytes ||
        dwords > destination.bytes / 4 ||
        overlaps(destination, queue.ring) || overlaps(destination, queue.fence)) {
        return QueueResult::Invalid;
    }
    if (!queue.io.mapped(queue.io.registers.context, destination, true)) {
        return QueueResult::MappingMissing;
    }
    if (!queue.io.cache(queue.io.registers.context, destination, CacheOperation::Invalidate)) {
        return fail(queue, QueueResult::IoError);
    }
    queue.io.registers.barrier(queue.io.registers.context);
    for (u32 index = 0; index < dwords; index++) {
        if (static_cast<volatile u32 *>(destination.cpu)[index] != value) {
            return fail(queue, QueueResult::DataMismatch);
        }
    }
    return QueueResult::Ok;
}

QueueResult haltQueue(Queue &queue)
{
    if (!queue.ownsHardware || !queue.io.registers.write) { return QueueResult::Invalid; }
    if (!writeGpu(queue, lumia950::kRegCpMeCntl, kMeHalt)) {
        return fail(queue, QueueResult::IoError);
    }
    queue.io.registers.barrier(queue.io.registers.context);
    u32 control = 0;
    u32 status = 0;
    if (!readGpu(queue, lumia950::kRegCpMeCntl, &control) ||
        !readGpu(queue, lumia950::kRegRbbmStatus, &status)) {
        return fail(queue, QueueResult::IoError);
    }
    if ((control & kMeHalt) == 0 || (status & kGpuBusy) != 0) {
        return fail(queue, QueueResult::Busy);
    }
    queue.state = QueueState::Halted;
    queue.ownsHardware = false;
    return QueueResult::Ok;
}

const char *queueResultName(QueueResult result)
{
    switch (result) {
    case QueueResult::Ok: return "OK";
    case QueueResult::Invalid: return "INVALID";
    case QueueResult::NotReady: return "NOT_READY";
    case QueueResult::MappingMissing: return "DMA_MAPPING_MISSING";
    case QueueResult::IoError: return "IO_ERROR";
    case QueueResult::Busy: return "GPU_BUSY";
    case QueueResult::NoRoom: return "RING_FULL";
    case QueueResult::GpuFault: return "GPU_FAULT";
    case QueueResult::TimedOut: return "TIMED_OUT";
    case QueueResult::DataMismatch: return "DATA_MISMATCH";
    }
    return "UNKNOWN";
}

}
