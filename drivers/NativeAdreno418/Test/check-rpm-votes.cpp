#include "RpmVotes.h"
#include <cstdio>
#include <cstring>

using namespace adreno;
using namespace adreno::rpm;

namespace {
unsigned checks;
unsigned failures;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", message); }
}

u32 word(const u8 *bytes)
{
    return bytes[0] | (static_cast<u32>(bytes[1]) << 8) |
        (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
}

struct Peer {
    u8 packet[252] = {};
    u32 size = 0;
    u32 sends = 0;
    bool noAck = false;
    bool blocked = false;
    static Transfer send(void *context, const u8 *bytes, u32 count)
    {
        auto &peer = *static_cast<Peer *>(context);
        if (peer.blocked) { return Transfer::WouldBlock; }
        ++peer.sends;
        if (count > sizeof(peer.packet)) { return Transfer::IoError; }
        std::memcpy(peer.packet, bytes, count);
        peer.size = count;
        return Transfer::Ok;
    }
    static Transfer receive(void *context, u8 *bytes, u32 capacity, u32 *size)
    {
        auto &peer = *static_cast<Peer *>(context);
        if (peer.noAck || !peer.size) { return Transfer::WouldBlock; }
        if (capacity < 20) { return Transfer::IoError; }
        const u32 words[] = {0x00716572, 12, 0x2367736d, 4, word(peer.packet + 8)};
        for (u32 index = 0; index < 5; ++index) {
            for (u32 byte = 0; byte < 4; ++byte) { bytes[index * 4 + byte] = static_cast<u8>(words[index] >> (byte * 8)); }
        }
        *size = 20;
        return Transfer::Ok;
    }
    static void delay(void *, u32) {}
    Transport io() { return {this, send, receive, delay}; }
};

constexpr u32 kEnable = 0x6c626e65;

struct Fixture {
    Peer peer;
    Session session = {};
    VoteBank bank = {};
    VoteSpec spec = {{0x54534554, 4}, Set::Active, {{kRateKey, 100}, {kEnable, 1}}, 2, true};
    Fixture()
    {
        expect(bindSession(session, peer.io(), 0x418, true) == Result::Ok, "bind serialized session for leased votes");
        expect(bindVotes(bank, session, &spec, 1, true, true) == VoteResult::Ok,
            "bind explicit inherited baseline without publishing or cancelling a vote");
    }
    VoteResult acquire(u32 owner, u32 rate, u32 enabled = 1)
    {
        const Value values[] = {{kEnable, enabled}, {kRateKey, rate}};
        return acquireVote(bank, 0, owner, values, 2, 8, 1);
    }
    VoteResult release(u32 owner, bool quiescent = true)
    { return releaseVote(bank, 0, owner, quiescent, 8, 1); }
};

void testAggregation()
{
    Fixture fixture;
    expect(fixture.peer.sends == 0 && fixture.bank.slots[0].acknowledged[0].value == 100,
        "binding a baseline is metadata only, not a fictitious readback or a zero vote");
    expect(fixture.acquire(1, 200) == VoteResult::Ok && fixture.peer.sends == 1 &&
        word(fixture.peer.packet + 28) == kRateKey && word(fixture.peer.packet + 36) == 200 &&
        word(fixture.peer.packet + 40) == kEnable && word(fixture.peer.packet + 48) == 1,
        "unordered client keys normalize to one atomic, canonical resource request");
    expect(fixture.acquire(1, 150) == VoteResult::NotReady && fixture.peer.sends == 1 && voteHeld(fixture.bank, 0, 1),
        "a live consumer cannot lower its lease through acquire; lowering requires quiescent release");
    expect(fixture.acquire(2, 150) == VoteResult::Ok && fixture.peer.sends == 1,
        "another owner's lower request does not lower the aggregate or create redundant traffic");
    expect(fixture.release(1, false) == VoteResult::NotReady && fixture.peer.sends == 1,
        "quiescence is mandatory even when a second owner still holds a lease");
    expect(fixture.release(1) == VoteResult::Ok && fixture.peer.sends == 2 &&
        fixture.bank.slots[0].acknowledged[0].value == 150 && voteHeld(fixture.bank, 0, 2),
        "releasing the maximum owner preserves the remaining owner's requirement");
    expect(fixture.acquire(2, 300, 2) == VoteResult::Ok && fixture.peer.sends == 3,
        "an existing lease can increase every dimension without losing identity");
    expect(fixture.acquire(3, 200, 4) == VoteResult::Ok && fixture.peer.sends == 4 &&
        fixture.bank.slots[0].acknowledged[0].value == 300 && fixture.bank.slots[0].acknowledged[1].value == 4,
        "max aggregation is independent for each qualified scalar key");
    expect(fixture.release(2) == VoteResult::Ok && fixture.peer.sends == 5 &&
        fixture.bank.slots[0].acknowledged[0].value == 200 && fixture.bank.slots[0].acknowledged[1].value == 4,
        "partial-owner release preserves another owner's stronger second key");
    expect(fixture.release(3) == VoteResult::Ok && fixture.peer.sends == 6 &&
        fixture.bank.slots[0].acknowledged[0].value == 100 && fixture.bank.slots[0].acknowledged[1].value == 1,
        "last-owner release restores the real nonzero baseline, not a destructive blanket cancel");
    expect(fixture.acquire(4, 50, 0) == VoteResult::Ok && fixture.peer.sends == 6 &&
        fixture.release(4) == VoteResult::Ok && fixture.peer.sends == 6,
        "below-baseline leases cannot undercut inherited requirements on acquisition or release");
    expect(fixture.release(4) == VoteResult::NotHeld && !voteHeld(fixture.bank, 0, 4),
        "double release does not manufacture ownership or additional RPM traffic");
    for (u32 owner = 1; owner <= kVoteOwners; ++owner) {
        expect(fixture.acquire(owner, 100) == VoteResult::Ok, "all bounded owner slots can hold independent baseline leases");
    }
    expect(fixture.acquire(9, 500) == VoteResult::NoRoom && fixture.peer.sends == 6,
        "ninth owner refuses before publication rather than evicting an existing consumer");
    expect(fixture.acquire(8, 500) == VoteResult::Ok && fixture.peer.sends == 7,
        "an existing owner can still increase its own lease when the bank is full");
    for (u32 owner = 1; owner <= kVoteOwners; ++owner) {
        expect(fixture.release(owner) == VoteResult::Ok, "every held lease releases without dropping another owner");
    }
    expect(fixture.peer.sends == 8 && fixture.bank.slots[0].acknowledged[0].value == 100,
        "only the last maximum owner requires a final baseline restoration");
}

void testUncertainPublication()
{
    Fixture fixture;
    fixture.peer.noAck = true;
    expect(fixture.acquire(1, 200) == VoteResult::RpmFailure && fixture.bank.state == VoteState::Poisoned &&
        fixture.session.state == State::Poisoned && fixture.peer.sends == 1,
        "lost ACK poisons both layers without sending the same transaction twice");
    expect(!voteHeld(fixture.bank, 0, 1) && fixture.bank.slots[0].acknowledged[0].value == 100 &&
        fixture.bank.pendingCount == 2 && fixture.bank.pendingOwner == 1 && fixture.bank.pendingValues[0].value == 200,
        "uncertain proposal remains recorded separately from last acknowledged aggregate and lease state");
    fixture.peer.noAck = false;
    expect(fixture.release(1) == VoteResult::NotReady && fixture.acquire(1, 250) == VoteResult::NotReady &&
        fixture.peer.sends == 1, "late ACK cannot revive a poisoned bank or authorize rollback");
    Fixture release;
    expect(release.acquire(1, 200) == VoteResult::Ok, "acquire lease for uncertain release test");
    release.peer.noAck = true;
    expect(release.release(1) == VoteResult::RpmFailure && voteHeld(release.bank, 0, 1) &&
        release.bank.slots[0].acknowledged[0].value == 200 && release.bank.pendingValues[0].value == 100,
        "unacknowledged release retains the last known lease and records the uncertain baseline proposal");
    Fixture blocked;
    blocked.peer.blocked = true;
    expect(blocked.acquire(1, 200) == VoteResult::RpmFailure && blocked.bank.state == VoteState::Ready &&
        blocked.session.state == State::Ready && !blocked.bank.pendingCount && !voteHeld(blocked.bank, 0, 1),
        "backpressure before publication preserves a retryable, unchanged bank");
    blocked.peer.blocked = false;
    expect(blocked.acquire(1, 200) == VoteResult::Ok && word(blocked.peer.packet + 8) == 0x419,
        "a prepublication retry uses a fresh message ID instead of reusing an earlier epoch ID");
}

void testValidation()
{
    for (u32 index = 0; index < 10; ++index) {
        Peer peer;
        Session session = {};
        VoteBank bank = {};
        VoteSpec specs[] = {{{0x54534554, 1}, Set::Active, {{1, 10}, {2, 20}}, 2, true},
            {{0x54534554, 2}, Set::Active, {{1, 10}}, 1, true}};
        expect(bindSession(session, peer.io(), 1, true) == Result::Ok, "validation fixture session binds");
        if (index == 0) { specs[0].count = 5; }
        if (index == 1) { specs[0].count = 0; }
        if (index == 2) { specs[0].baseline[1].key = 1; }
        if (index == 3) { specs[0].baseline[0].key = 0; }
        if (index == 4) { specs[0].maximumAggregationQualified = false; }
        if (index == 5) { specs[0].resource.type = 0; }
        if (index == 6) { specs[0].set = static_cast<Set>(2); }
        if (index == 7) { specs[1] = specs[0]; }
        const VoteResult result = bindVotes(bank, session, specs, 2, index != 8, index != 9);
        expect(result != VoteResult::Ok && bank.state == VoteState::Empty && peer.sends == 0,
            "unknown aggregation, duplicate keys/resources and unqualified baselines refuse atomically");
    }
    Fixture fixture;
    const Value duplicates[] = {{kRateKey, 200}, {kRateKey, 300}};
    const Value unknown[] = {{kRateKey, 200}, {0xdead, 1}};
    expect(acquireVote(fixture.bank, 0, 1, duplicates, 2, 8, 1) == VoteResult::Invalid &&
        acquireVote(fixture.bank, 0, 1, unknown, 2, 8, 1) == VoteResult::Invalid,
        "duplicate and unknown client keys never reach RPM");
    expect(fixture.acquire(0, 200) == VoteResult::Invalid &&
        acquireVote(fixture.bank, 1, 1, unknown, 2, 8, 1) == VoteResult::Invalid && fixture.peer.sends == 0,
        "zero owner and out-of-range resource cannot mutate leases");
    fixture.bank.slots[0].spec.count = 0xffffffffu;
    expect(fixture.acquire(1, 200) == VoteResult::Invalid && fixture.release(1) == VoteResult::Invalid,
        "corrupted public resource count is refused before any key-array access");
    fixture.bank.count = 0xffffffffu;
    expect(fixture.acquire(1, 200) == VoteResult::NotReady && fixture.release(1) == VoteResult::NotReady &&
        !voteHeld(fixture.bank, 0, 1), "corrupted public bank count cannot turn into an out-of-bounds owner scan");
}
}

int main()
{
    testAggregation();
    testUncertainPublication();
    testValidation();
    std::printf("RPM leased votes: %u checks, %u failures (simulated peer)\n", checks, failures);
    return failures ? 1 : 0;
}
