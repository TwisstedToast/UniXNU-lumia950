#ifndef NATIVE_ADRENO418_GPU_BATCH_H
#define NATIVE_ADRENO418_GPU_BATCH_H

#include "A418Init.h"
#include "PowerController.h"
#include "SmdMemory.h"
#include "SmmuContext.h"
#include "SmmuRoutes.h"
#include "Submission.h"

namespace adreno {

struct BatchIo {
    RegisterIo registers;
    rpm::MemoryAccess memory;
    bool (*cleanTables)(void *, const lpae::Tables &);
    bool (*cache)(void *, const DmaRegion &, CacheOperation);
};
struct BatchBuffers {
    lpae::Tables tables;
    DmaRegion ring;
    DmaRegion fence;
    DmaRegion destination;
    const u32 *pm4;
    u32 pm4Words;
    const u32 *pfp;
    u32 pfpWords;
};
struct BatchPlan {
    rpm::MemoryLayout memory;
    rpm::VoteSpec resources[rpm::kVoteResources];
    u32 resourceCount;
    power::Plan power;
    HardwareContract hardware;
    smmu::RouteContract routes;
    u32 firstMessageId;
    u32 fillValue;
    u32 fillDwords;
    bool firmwareExited;
    bool exclusiveChannel;
    bool messageEpochQualified;
    bool baselineQualified;
    bool dmaOwnershipQualified;
    bool firmwareDigestsQualified;
};
struct BatchBudget { u32 rpmPolls; u32 rpmDelayUs; u32 mmioPolls; u32 gpuPolls; };
enum class BatchStage : u32 {
    Empty, Qualifying, Mapping, Observing, Claiming, Opening, Powering, Initializing,
    Routing, Binding, Translating, StartingQueue, WaitingInit, Filling, WaitingFill,
    Verifying, Halting, Disabling, StoppingHardware, StoppingPower, Closing, Retiring,
    Complete, Failed
};
enum class BatchResult : u32 {
    Ok, Invalid, Unqualified, NotReady, MappingFailed, MemoryFailed, ChannelFailed,
    PowerFailed, HardwareFailed, RouteFailed, ContextFailed, QueueFailed, ReadbackFailed
};
struct GpuBatch {
    BatchIo io;
    BatchBuffers buffers;
    BatchStage stage;
    BatchStage failureStage;
    BatchResult result;
    rpm::Result rpmResult;
    power::Result powerResult;
    HardwareResult hardwareResult;
    smmu::RouteResult routeResult;
    ContextResult contextResult;
    QueueResult queueResult;
    rpm::MemoryBinding memory;
    rpm::SmdPipe pipe;
    rpm::Session session;
    rpm::VoteBank votes;
    power::Controller power;
    CommandHardware hardware;
    smmu::Routes routes;
    ContextBank contexts[2];
    Queue queue;
    u32 verifiedPasses;
    bool resourcesPinned;
    bool dmaReusable;
};

BatchResult runGpuBatch(GpuBatch &batch, const BatchIo &io, const BatchBuffers &buffers,
    const BatchPlan &plan, const BatchBudget &budget);
const char *batchStageName(BatchStage stage);

}
#endif
