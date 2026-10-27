#include "PowerController.h"
#include "AdrenoCore.h"
#include "BoardConstants.h"

namespace adreno {
namespace power {
namespace {

bool validBudget(u32 polls, u32 delayUs)
{
    return polls && polls <= 100000 && delayUs <= 1000 &&
        static_cast<u64>(polls) * delayUs <= 5000000;
}

Result fail(Controller &controller, Result result)
{
    controller.state = State::Poisoned;
    return result;
}

bool read(Controller &controller, u32 offset, u32 &value)
{
    return controller.io.read(controller.io.context, RegisterSpace::Mmss, offset, &value) &&
        value != 0xffffffffu;
}

bool write(Controller &controller, u32 offset, u32 value)
{
    if (!controller.io.write(controller.io.context, RegisterSpace::Mmss, offset, value)) { return false; }
    controller.io.barrier(controller.io.context);
    return true;
}

Result domain(Controller &controller, u32 offset, bool enabled, u32 budget)
{
    u32 current = 0;
    if (!read(controller, offset, current)) { return Result::IoError; }
    if (gdscrUnderHardwareControl(current)) { return Result::Unqualified; }
    const u32 wanted = enabled ? gdscrEnableValue(current) : gdscrDisableValue(current);
    if (!write(controller, offset, wanted)) { return Result::IoError; }
    for (u32 attempt = 0; attempt < budget; ++attempt) {
        if (!read(controller, offset, current)) { return Result::IoError; }
        ++controller.polls;
        if (((current & lumia950::kGdscrPowerOn) != 0) == enabled &&
            ((current & kGdscrSwCollapse) == 0) == enabled) {
            controller.io.delayUs(controller.io.context, 1);
            return Result::Ok;
        }
        if (attempt + 1 < budget) { controller.io.delayUs(controller.io.context, 1); }
    }
    return Result::TimedOut;
}

Result branch(Controller &controller, u32 offset, bool enabled, u32 budget)
{
    u32 current = 0;
    if (!read(controller, offset, current)) { return Result::IoError; }
    const u32 wanted = enabled ? current | kBranchEnable : current & ~kBranchEnable;
    if (!write(controller, offset, wanted)) { return Result::IoError; }
    for (u32 attempt = 0; attempt < budget; ++attempt) {
        if (!read(controller, offset, current)) { return Result::IoError; }
        ++controller.polls;
        if (((current & kBranchEnable) != 0) == enabled &&
            ((current & kBranchOff) == 0) == enabled) { return Result::Ok; }
        if (attempt + 1 < budget) { controller.io.delayUs(controller.io.context, 1); }
    }
    return Result::TimedOut;
}

Result retention(Controller &controller, bool enabled)
{
    u32 current = 0;
    if (!read(controller, kCoreClockOffset, current)) { return Result::IoError; }
    const u32 bits = enabled ? kCoreRetentionMask : controller.initial.coreClock & kCoreRetentionMask;
    if (!write(controller, kCoreClockOffset, (current & ~kCoreRetentionMask) | bits) ||
        !read(controller, kCoreClockOffset, current)) { return Result::IoError; }
    return (current & kCoreRetentionMask) == bits ? Result::Ok : Result::NotReady;
}

bool valuesMatch(const Vote &vote, const rpm::VoteSpec &spec)
{
    if (vote.count != spec.count || !vote.count || vote.count > rpm::kVoteValues) { return false; }
    bool positive = false;
    for (u32 index = 0; index < vote.count; ++index) {
        bool found = false;
        for (u32 key = 0; key < spec.count; ++key) {
            if (vote.values[index].key == spec.baseline[key].key) { found = true; }
        }
        if (!found) { return false; }
        for (u32 previous = 0; previous < index; ++previous) {
            if (vote.values[index].key == vote.values[previous].key) { return false; }
        }
        positive |= vote.values[index].value != 0;
    }
    return positive;
}

bool resourceIs(const rpm::VoteSpec &spec, rpm::Resource resource)
{
    return spec.set == rpm::Set::Active && spec.resource.type == resource.type &&
        spec.resource.id == resource.id;
}

}

bool validPlan(const Plan &plan, const rpm::VoteBank &bank)
{
    if (!plan.boardProfileQualified || !plan.exclusiveHardware || !plan.owner ||
        plan.count < 4 || plan.count > rpm::kVoteResources ||
        !plan.maximumGraphicsKHz || plan.maximumGraphicsKHz > lumia950::kMaxGpuHz / 1000 ||
        !bank.count || bank.count > rpm::kVoteResources ||
        bank.state != rpm::VoteState::Ready || !bank.session || bank.session->state != rpm::State::Ready) {
        return false;
    }
    bool roles[4] = {};
    bool configurationClock = false;
    for (u32 resource = 0; resource < bank.count; ++resource) {
        if (rpm::voteHeld(bank, resource, plan.owner)) { return false; }
    }
    for (u32 index = 0; index < plan.count; ++index) {
        const Vote &vote = plan.votes[index];
        const u32 role = static_cast<u32>(vote.role);
        if (role > 3 || vote.resource >= bank.count ||
            (index && role < static_cast<u32>(plan.votes[index - 1].role))) { return false; }
        const rpm::VoteSpec &spec = bank.slots[vote.resource].spec;
        if (spec.set != rpm::Set::Active || !valuesMatch(vote, spec)) { return false; }
        for (u32 previous = 0; previous < index; ++previous) {
            if (vote.resource == plan.votes[previous].resource) { return false; }
        }
        if (vote.role == VoteRole::ConfigurationBus && resourceIs(spec, rpm::kMmssAhbParent)) {
            configurationClock = spec.count == 1 && spec.baseline[0].key == rpm::kRateKey;
        }
        if (vote.role == VoteRole::GraphicsClock) {
            if (roles[role] || !resourceIs(spec, rpm::kGraphicsParent) || spec.count != 1 ||
                spec.baseline[0].key != rpm::kRateKey ||
                vote.values[0].value > plan.maximumGraphicsKHz ||
                bank.slots[vote.resource].acknowledged[0].value > plan.maximumGraphicsKHz) { return false; }
        }
        roles[role] = true;
    }
    return roles[0] && roles[1] && roles[2] && roles[3] && configurationClock;
}

Result start(Controller &controller, const RegisterIo &io, rpm::VoteBank &bank,
             const Plan &plan, u32 rpmPolls, u32 rpmDelayUs, u32 mmioPolls)
{
    if (controller.state != State::Empty) { return Result::NotReady; }
    if (!plan.boardProfileQualified || !plan.exclusiveHardware) { return Result::Unqualified; }
    if (!validPlan(plan, bank) || !io.read || !io.write || !io.barrier || !io.delayUs ||
        rpmPolls < 2 || !validBudget(rpmPolls, rpmDelayUs) || !validBudget(mmioPolls, 1)) { return Result::Invalid; }
    PowerSnapshot initial = {};
    const ProbeResult sampled = readPower(io, &initial);
    if (sampled == ProbeResult::IoError || sampled == ProbeResult::NoResponse) { return Result::IoError; }
    const u32 domains[] = {initial.cx, initial.gx};
    for (u32 value : domains) {
        if ((value & lumia950::kGdscrPowerOn) || !(value & kGdscrSwCollapse) ||
            gdscrUnderHardwareControl(value)) { return Result::Unqualified; }
    }
    if ((initial.coreClock | initial.ahbClock | initial.timerClock) & kBranchEnable) {
        return Result::Unqualified;
    }
    controller.io = io;
    controller.bank = &bank;
    controller.plan = plan;
    controller.initial = initial;
    controller.state = State::Voting;
    controller.acquired = 0;
    controller.polls = 0;
    for (u32 index = 0; index < plan.count; ++index) {
        const Vote &vote = plan.votes[index];
        controller.lastVote = rpm::acquireVote(bank, vote.resource, plan.owner, vote.values,
            vote.count, rpmPolls, rpmDelayUs);
        if (controller.lastVote != rpm::VoteResult::Ok) { return fail(controller, Result::RpmFailure); }
        ++controller.acquired;
    }
    controller.state = State::Enabling;
    const u32 domainOffsets[] = {lumia950::kGdscrCxOffset, lumia950::kGdscrGxOffset};
    for (u32 offset : domainOffsets) {
        const Result result = domain(controller, offset, true, mmioPolls);
        if (result != Result::Ok) { return fail(controller, result); }
    }
    const Result retained = retention(controller, true);
    if (retained != Result::Ok) { return fail(controller, retained); }
    const u32 branches[] = {kAhbClockOffset, kTimerClockOffset, kCoreClockOffset};
    for (u32 offset : branches) {
        const Result result = branch(controller, offset, true, mmioPolls);
        if (result != Result::Ok) { return fail(controller, result); }
    }
    if (readPower(io, &controller.final) != ProbeResult::Ready) { return fail(controller, Result::NotReady); }
    controller.state = State::Ready;
    return Result::Ok;
}

Result stop(Controller &controller, bool consumersQuiescent,
            u32 rpmPolls, u32 rpmDelayUs, u32 mmioPolls)
{
    if (controller.state != State::Ready || !consumersQuiescent) { return Result::NotReady; }
    if (rpmPolls < 2 || !validBudget(rpmPolls, rpmDelayUs) || !validBudget(mmioPolls, 1)) { return Result::Invalid; }
    controller.state = State::Stopping;
    const u32 branches[] = {kCoreClockOffset, kTimerClockOffset, kAhbClockOffset};
    for (u32 offset : branches) {
        const Result result = branch(controller, offset, false, mmioPolls);
        if (result != Result::Ok) { return fail(controller, result); }
    }
    const u32 domains[] = {lumia950::kGdscrGxOffset, lumia950::kGdscrCxOffset};
    for (u32 offset : domains) {
        const Result result = domain(controller, offset, false, mmioPolls);
        if (result != Result::Ok) { return fail(controller, result); }
    }
    const Result restored = retention(controller, false);
    if (restored != Result::Ok) { return fail(controller, restored); }
    if (readPower(controller.io, &controller.final) != ProbeResult::DomainOff ||
        ((controller.final.gx | controller.final.cx) & lumia950::kGdscrPowerOn) ||
        ((controller.final.coreClock | controller.final.ahbClock | controller.final.timerClock) & kBranchEnable)) {
        return fail(controller, Result::NotReady);
    }
    while (controller.acquired) {
        const Vote &vote = controller.plan.votes[controller.acquired - 1];
        controller.lastVote = rpm::releaseVote(*controller.bank, vote.resource, controller.plan.owner,
            true, rpmPolls, rpmDelayUs);
        if (controller.lastVote != rpm::VoteResult::Ok) { return fail(controller, Result::RpmFailure); }
        --controller.acquired;
    }
    controller.state = State::Stopped;
    return Result::Ok;
}

}
}
