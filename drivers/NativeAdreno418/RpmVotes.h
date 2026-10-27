#ifndef NATIVE_ADRENO418_RPM_VOTES_H
#define NATIVE_ADRENO418_RPM_VOTES_H

#include "RpmCore.h"

namespace adreno {
namespace rpm {

constexpr u32 kVoteResources = 8;
constexpr u32 kVoteOwners = 8;
constexpr u32 kVoteValues = 4;

struct VoteSpec {
    Resource resource;
    Set set;
    Value baseline[kVoteValues];
    u32 count;
    bool maximumAggregationQualified;
};
struct VoteLease { u32 owner; Value values[kVoteValues]; };
struct VoteSlot {
    VoteSpec spec;
    Value acknowledged[kVoteValues];
    VoteLease leases[kVoteOwners];
};
enum class VoteState : u32 { Empty, Ready, Publishing, Poisoned };
enum class VoteResult : u32 { Ok, Invalid, Unqualified, NotReady, NoRoom, NotHeld, RpmFailure };
struct VoteBank {
    Session *session;
    VoteSlot slots[kVoteResources];
    u32 count;
    VoteState state;
    Result lastRpmResult;
    u32 pendingResource;
    u32 pendingOwner;
    Value pendingValues[kVoteValues];
    u32 pendingCount;
};

VoteResult bindVotes(VoteBank &bank, Session &session, const VoteSpec *specs, u32 count,
                     bool baselineQualified, bool exclusiveOwnership);
VoteResult acquireVote(VoteBank &bank, u32 resource, u32 owner,
                       const Value *values, u32 count, u32 polls, u32 delayUs);
VoteResult releaseVote(VoteBank &bank, u32 resource, u32 owner,
                       bool consumerQuiescent, u32 polls, u32 delayUs);
bool voteHeld(const VoteBank &bank, u32 resource, u32 owner);

}
}
#endif
