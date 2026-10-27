#ifndef NATIVE_ADRENO418_POWER_CONTROLLER_H
#define NATIVE_ADRENO418_POWER_CONTROLLER_H

#include "GpuIo.h"
#include "RpmVotes.h"

namespace adreno {
namespace power {

constexpr u32 kCoreRetentionMask = 0x6000;

enum class VoteRole : u32 { Supply, ConfigurationBus, MemoryBus, GraphicsClock };
struct Vote {
    VoteRole role;
    u32 resource;
    rpm::Value values[rpm::kVoteValues];
    u32 count;
};
struct Plan {
    Vote votes[rpm::kVoteResources];
    u32 count;
    u32 owner;
    u32 maximumGraphicsKHz;
    bool boardProfileQualified;
    bool exclusiveHardware;
};
enum class State : u32 { Empty, Voting, Enabling, Ready, Stopping, Stopped, Poisoned };
enum class Result : u32 { Ok, Invalid, Unqualified, NotReady, RpmFailure, IoError, TimedOut };
struct Controller {
    RegisterIo io;
    rpm::VoteBank *bank;
    Plan plan;
    PowerSnapshot initial;
    PowerSnapshot final;
    State state;
    u32 acquired;
    u32 polls;
    rpm::VoteResult lastVote;
};

struct PinnedDependencies {
    u32 graphicsMicrovolts;
    u32 graphicsKHz;
    bool observedRailReady;
    bool clockAcknowledged;
    bool inheritedBusesUntouched;
    bool exclusiveHardware;
    bool rpmManagedSupply = false;
};

bool validPlan(const Plan &plan, const rpm::VoteBank &bank);
Result start(Controller &controller, const RegisterIo &io, rpm::VoteBank &bank,
             const Plan &plan, u32 rpmPolls, u32 rpmDelayUs, u32 mmioPolls);
Result stop(Controller &controller, bool consumersQuiescent,
            u32 rpmPolls, u32 rpmDelayUs, u32 mmioPolls);
Result startPinned(Controller &controller, const RegisterIo &io,
                   const PinnedDependencies &dependencies, u32 mmioPolls);

}
}
#endif
