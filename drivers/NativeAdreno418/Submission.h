#ifndef NATIVE_ADRENO418_SUBMISSION_H
#define NATIVE_ADRENO418_SUBMISSION_H

#include "CommandStream.h"
#include "GpuIo.h"

namespace adreno {

struct DmaRegion {
    u32 *cpu;
    u64 physical;
    u32 gpu;
    u32 bytes;
};

enum class CacheOperation : u32 { Clean, Invalidate };

struct SubmissionIo {
    RegisterIo registers;
    bool (*mapped)(void *, const DmaRegion &, bool writable);
    bool (*cache)(void *, const DmaRegion &, CacheOperation);
};

enum class QueueState : u32 { Empty, Initializing, Ready, Pending, Failed, Halted };
enum class QueueResult : u32 {
    Ok, Invalid, NotReady, MappingMissing, IoError, Busy, NoRoom, GpuFault,
    TimedOut, DataMismatch
};

struct Queue {
    SubmissionIo io;
    DmaRegion ring;
    DmaRegion fence;
    DmaRegion destination;
    QueueState state;
    u32 writePointer;
    u32 sequence;
    u32 fault;
    u32 polls;
    u32 fillValue;
    u32 fillDwords;
    bool ownsHardware;
};

QueueResult initializeQueue(Queue &queue, const SubmissionIo &io,
    const DmaRegion &ring, const DmaRegion &fence,
    const u32 *pm4, u32 pm4Words, const u32 *pfp, u32 pfpWords,
    bool exclusiveOwnership);
QueueResult submitFill(Queue &queue, const DmaRegion &destination,
                       u32 value, u32 dwords);
QueueResult waitQueue(Queue &queue, u32 pollBudget, u32 delayUs);
QueueResult verifyFill(Queue &queue, const DmaRegion &destination, u32 value, u32 dwords);
QueueResult haltQueue(Queue &queue);
const char *queueResultName(QueueResult result);

}
#endif
