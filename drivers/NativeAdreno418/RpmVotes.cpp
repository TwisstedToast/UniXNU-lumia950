#include "RpmVotes.h"

namespace adreno {
namespace rpm {
namespace {

bool sameResource(const VoteSpec &first, const VoteSpec &second)
{
    return first.resource.type == second.resource.type && first.resource.id == second.resource.id &&
        first.set == second.set;
}

bool validSpec(const VoteSpec &spec)
{
    if (!spec.resource.type || (spec.set != Set::Active && spec.set != Set::Sleep) ||
        !spec.count || spec.count > kVoteValues || !spec.maximumAggregationQualified) { return false; }
    for (u32 index = 0; index < spec.count; ++index) {
        if (!spec.baseline[index].key) { return false; }
        for (u32 previous = 0; previous < index; ++previous) {
            if (spec.baseline[index].key == spec.baseline[previous].key) { return false; }
        }
    }
    return true;
}

bool normalize(const VoteSpec &spec, const Value *values, u32 count, Value *output)
{
    if (!validSpec(spec) || !values || count != spec.count) { return false; }
    for (u32 index = 0; index < count; ++index) {
        bool found = false;
        for (u32 input = 0; input < count; ++input) {
            if (values[input].key != spec.baseline[index].key) { continue; }
            if (found) { return false; }
            output[index] = values[input];
            found = true;
        }
        if (!found) { return false; }
    }
    return true;
}

void aggregate(VoteSlot &slot)
{
    for (u32 index = 0; index < slot.spec.count; ++index) {
        slot.acknowledged[index] = slot.spec.baseline[index];
        for (const VoteLease &lease : slot.leases) {
            if (lease.owner && lease.values[index].value > slot.acknowledged[index].value) {
                slot.acknowledged[index].value = lease.values[index].value;
            }
        }
    }
}

VoteResult publish(VoteBank &bank, u32 resource, u32 owner, VoteSlot &candidate,
                   u32 polls, u32 delayUs)
{
    if (polls < 2 || polls > 100000 || delayUs > 1000 ||
        static_cast<u64>(polls) * delayUs > 5000000) { return VoteResult::Invalid; }
    aggregate(candidate);
    bool changed = false;
    for (u32 index = 0; index < candidate.spec.count; ++index) {
        if (candidate.acknowledged[index].value != bank.slots[resource].acknowledged[index].value) {
            changed = true;
        }
    }
    if (!changed) { bank.slots[resource] = candidate; return VoteResult::Ok; }
    bank.state = VoteState::Publishing;
    bank.pendingResource = resource;
    bank.pendingOwner = owner;
    bank.pendingCount = candidate.spec.count;
    for (u32 index = 0; index < candidate.spec.count; ++index) {
        bank.pendingValues[index] = candidate.acknowledged[index];
    }
    bank.lastRpmResult = transact(*bank.session, candidate.spec.set, candidate.spec.resource,
        candidate.acknowledged, candidate.spec.count, polls, delayUs);
    if (bank.lastRpmResult != Result::Ok) {
        if (bank.session->state == State::Ready) {
            bank.state = VoteState::Ready;
            bank.pendingCount = 0;
        } else { bank.state = VoteState::Poisoned; }
        return VoteResult::RpmFailure;
    }
    bank.slots[resource] = candidate;
    bank.pendingCount = 0;
    bank.state = VoteState::Ready;
    return VoteResult::Ok;
}

bool ready(const VoteBank &bank)
{
    return bank.state == VoteState::Ready && bank.count && bank.count <= kVoteResources &&
        bank.session && bank.session->state == State::Ready;
}

}

VoteResult bindVotes(VoteBank &bank, Session &session, const VoteSpec *specs, u32 count,
                     bool baselineQualified, bool exclusiveOwnership)
{
    if (bank.state != VoteState::Empty || session.state != State::Ready) { return VoteResult::NotReady; }
    if (!baselineQualified || !exclusiveOwnership) { return VoteResult::Unqualified; }
    if (!specs || !count || count > kVoteResources) { return VoteResult::Invalid; }
    for (u32 index = 0; index < count; ++index) {
        if (!validSpec(specs[index])) { return VoteResult::Invalid; }
        for (u32 previous = 0; previous < index; ++previous) {
            if (sameResource(specs[index], specs[previous])) { return VoteResult::Invalid; }
        }
    }
    bank.session = &session;
    bank.count = count;
    for (u32 index = 0; index < count; ++index) {
        bank.slots[index] = {};
        bank.slots[index].spec = specs[index];
        aggregate(bank.slots[index]);
    }
    bank.state = VoteState::Ready;
    bank.pendingCount = 0;
    bank.lastRpmResult = Result::Ok;
    return VoteResult::Ok;
}

VoteResult acquireVote(VoteBank &bank, u32 resource, u32 owner,
                       const Value *values, u32 count, u32 polls, u32 delayUs)
{
    if (!ready(bank)) { return VoteResult::NotReady; }
    if (resource >= bank.count || !owner) { return VoteResult::Invalid; }
    VoteSlot candidate = bank.slots[resource];
    u32 target = kVoteOwners;
    for (u32 index = 0; index < kVoteOwners; ++index) {
        if (candidate.leases[index].owner == owner) { target = index; break; }
        if (!candidate.leases[index].owner && target == kVoteOwners) { target = index; }
    }
    if (target == kVoteOwners) { return VoteResult::NoRoom; }
    const VoteLease previous = candidate.leases[target];
    if (!normalize(candidate.spec, values, count, candidate.leases[target].values)) { return VoteResult::Invalid; }
    for (u32 index = 0; previous.owner && index < candidate.spec.count; ++index) {
        if (candidate.leases[target].values[index].value < previous.values[index].value) {
            return VoteResult::NotReady;
        }
    }
    candidate.leases[target].owner = owner;
    return publish(bank, resource, owner, candidate, polls, delayUs);
}

VoteResult releaseVote(VoteBank &bank, u32 resource, u32 owner,
                       bool consumerQuiescent, u32 polls, u32 delayUs)
{
    if (!ready(bank)) { return VoteResult::NotReady; }
    if (resource >= bank.count || !owner) { return VoteResult::Invalid; }
    if (!consumerQuiescent) { return VoteResult::NotReady; }
    VoteSlot candidate = bank.slots[resource];
    if (!validSpec(candidate.spec)) { return VoteResult::Invalid; }
    for (VoteLease &lease : candidate.leases) {
        if (lease.owner != owner) { continue; }
        lease = {};
        return publish(bank, resource, owner, candidate, polls, delayUs);
    }
    return VoteResult::NotHeld;
}

bool voteHeld(const VoteBank &bank, u32 resource, u32 owner)
{
    if (bank.state == VoteState::Empty || bank.count > kVoteResources ||
        resource >= bank.count || !owner) { return false; }
    for (const VoteLease &lease : bank.slots[resource].leases) {
        if (lease.owner == owner) { return true; }
    }
    return false;
}

}
}
