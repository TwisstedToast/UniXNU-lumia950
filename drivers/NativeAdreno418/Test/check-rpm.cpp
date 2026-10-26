#include "RpmCore.h"
#include "SmdPipe.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

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

void append(std::vector<u8> &bytes, u32 value)
{
    bytes.push_back(static_cast<u8>(value & 255));
    bytes.push_back(static_cast<u8>((value >> 8) & 255));
    bytes.push_back(static_cast<u8>((value >> 16) & 255));
    bytes.push_back(static_cast<u8>(value >> 24));
}

u32 word(const u8 *bytes)
{
    return bytes[0] + bytes[1] * 256u + bytes[2] * 65536u + bytes[3] * 16777216u;
}

void replace(std::vector<u8> &bytes, u32 offset, u32 value)
{
    for (u32 index = 0; index < 4; ++index) { bytes[offset + index] = static_cast<u8>((value >> (index * 8)) & 255); }
}

std::vector<u8> ack(u32 messageId, const char *error = nullptr)
{
    std::vector<u8> result;
    for (u32 value : {0x00716572u, 12u, 0x2367736du, 4u, messageId}) { append(result, value); }
    if (error) {
        append(result, 0x00727265);
        append(result, static_cast<u32>(std::strlen(error) + 1));
        do { result.push_back(static_cast<u8>(*error)); } while (*error++);
        while (result.size() & 3) { result.push_back(0); }
        replace(result, 4, static_cast<u32>(result.size() - 8));
    }
    return result;
}

Ack decode(const std::vector<u8> &bytes, u32 expectedMessageId)
{
    return decodeAck(bytes.data(), static_cast<u32>(bytes.size()), expectedMessageId);
}

void testPackets()
{
    const Value rate = clockRate(300000000);
    expect(rate.key == 0x007a484b && rate.value == 300000, "graphics rate is a kHz vote, not raw Hz or an enable bit");
    expect(clockRate(0).value == 0 && clockRate(1).value == 1 &&
           clockRate(1001).value == 2 && clockRate(0xffffffff).value == 4294968,
           "rate rounding is upward and cannot overflow a 32-bit Hz input");
    expect(kGraphicsParent.type == 0x326b6c63 && kGraphicsParent.id == 1,
           "MSM8992 graphics parent is RPM clk2 resource 1");
    expect(kMmssAhbParent.type == 0x316b6c63 && kMmssAhbParent.id == 3,
           "MMSS NOC AHB parent is distinct RPM clk1 resource 3");
    Packet packet = {};
    expect(encodeRequest(packet, 7, Set::Active, kGraphicsParent, &rate, 1), "encode graphics clock vote");
    const u8 golden[] = {
        0x72,0x65,0x71,0x00, 0x20,0,0,0, 7,0,0,0, 0,0,0,0,
        0x63,0x6c,0x6b,0x32, 1,0,0,0, 12,0,0,0,
        0x4b,0x48,0x7a,0, 4,0,0,0, 0xe0,0x93,4,0
    };
    expect(packet.size == sizeof(golden) && !std::memcmp(packet.bytes, golden, sizeof(golden)),
           "request matches independent 40-byte little-endian golden message without trailing allocation bytes");
    expect(encodeRequest(packet, 8, Set::Sleep, kGraphicsParent, &rate, 1) && word(packet.bytes + 12) == 1,
           "sleep set is explicit and separate from active set");
    Value maximum[kMaximumValues];
    for (u32 index = 0; index < kMaximumValues; ++index) { maximum[index] = {index + 1, index}; }
    expect(encodeRequest(packet, 1, Set::Active, kGraphicsParent, maximum, kMaximumValues) && packet.size == 220,
           "maximum scalar KVP set fits the RPM packet bound");
    expect(!encodeRequest(packet, 0, Set::Active, kGraphicsParent, &rate, 1) && packet.size == 0,
           "zero message ID refuses and invalidates previous packet");
    expect(!encodeRequest(packet, 1, static_cast<Set>(2), kGraphicsParent, &rate, 1), "unknown resource set is refused");
    expect(!encodeRequest(packet, 1, Set::Active, {0, 1}, &rate, 1), "empty resource type is refused");
    expect(!encodeRequest(packet, 1, Set::Active, kGraphicsParent, nullptr, 1), "missing vote data is refused");
    expect(!encodeRequest(packet, 1, Set::Active, kGraphicsParent, &rate, 0), "empty vote is refused");
    expect(!encodeRequest(packet, 1, Set::Active, kGraphicsParent, &rate, 17), "KVP limit is checked before reading an oversized array");
    Value duplicates[] = {{1, 10}, {1, 20}};
    expect(!encodeRequest(packet, 1, Set::Active, kGraphicsParent, duplicates, 2), "duplicate keys cannot silently override a vote");
    const Value missingKey = {0, 1};
    expect(!encodeRequest(packet, 1, Set::Active, kGraphicsParent, &missingKey, 1), "zero key is refused");

    const auto success = ack(7);
    expect(decode(success, 7).kind == AckKind::Accepted, "matching msg# ACK is accepted");
    expect(decode(success, 8).kind == AckKind::Stale, "ACK for another request is not completion");
    for (u32 size = 0; size < success.size(); ++size) {
        expect(decodeAck(success.data(), size, 7).kind == AckKind::Malformed, "every truncated ACK is refused");
    }
    expect(decodeAck(nullptr, 20, 7).kind == AckKind::Malformed, "missing ACK is refused");
    expect(decodeAck(success.data(), 0xffffffff, 7).kind == AckKind::Malformed, "ACK bound is checked before any oversized read");
    for (const auto &mutation : std::vector<std::array<u32, 2>>{{0, 0}, {4, 8}, {4, 16}, {8, 0}, {12, 0}, {12, 8}, {16, 0}}) {
        auto malformed = success;
        replace(malformed, mutation[0], mutation[1]);
        expect(decode(malformed, 7).kind == AckKind::Malformed, "malformed header/key/length/ID never completes a request");
    }
    auto duplicate = success;
    duplicate.insert(duplicate.end(), success.begin() + 8, success.end());
    replace(duplicate, 4, static_cast<u32>(duplicate.size() - 8));
    expect(decode(duplicate, 7).kind == AckKind::Malformed, "duplicate msg# is refused");
    const auto rejected = ack(7, "resource does not exist");
    const Ack rejection = decode(rejected, 7);
    expect(rejection.kind == AckKind::RemoteError && !std::strcmp(rejection.error, "resource does not exist"),
           "remote resource error retains a bounded diagnostic instead of looking like success");
    expect(decode(rejected, 8).kind == AckKind::Stale, "another request's error does not identify the current request");
    const auto longError = ack(7, "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
    const Ack bounded = decode(longError, 7);
    expect(bounded.kind == AckKind::RemoteError && std::strlen(bounded.error) == 63, "long errors cannot overflow the diagnostic buffer");
}

struct FakeTransport {
    unsigned sends = 0, receives = 0, delays = 0;
    unsigned blockedSends = 0;
    u32 delayTotal = 0;
    Transfer sendStatus = Transfer::Ok;
    Transfer receiveStatus = Transfer::Ok;
    std::vector<u8> sent;
    std::vector<std::vector<u8>> responses;
    bool oversizedResponse = false;

    static Transfer send(void *context, const u8 *bytes, u32 size)
    {
        auto &device = *static_cast<FakeTransport *>(context);
        ++device.sends;
        if (device.blockedSends) { --device.blockedSends; return Transfer::WouldBlock; }
        if (device.sendStatus != Transfer::Ok) { return device.sendStatus; }
        device.sent.assign(bytes, bytes + size);
        return Transfer::Ok;
    }
    static Transfer receive(void *context, u8 *bytes, u32 capacity, u32 *size)
    {
        auto &device = *static_cast<FakeTransport *>(context);
        ++device.receives;
        if (device.receiveStatus != Transfer::Ok) { return device.receiveStatus; }
        if (device.responses.empty()) { return Transfer::WouldBlock; }
        const auto response = device.responses.front();
        device.responses.erase(device.responses.begin());
        if (response.size() > capacity) { return Transfer::IoError; }
        std::memcpy(bytes, response.data(), response.size());
        *size = device.oversizedResponse ? 0xffffffff : static_cast<u32>(response.size());
        return Transfer::Ok;
    }
    static void delay(void *context, u32 microseconds)
    {
        auto &device = *static_cast<FakeTransport *>(context);
        ++device.delays;
        device.delayTotal += microseconds;
    }
    Transport io() { return {this, send, receive, delay}; }
};

void testSessions()
{
    const Value rate = clockRate(27000000);
    FakeTransport device;
    Session session = {};
    expect(bindSession(session, device.io(), 1, false) == Result::Unowned, "RPM channel cannot be used without exclusive ownership");
    expect(bindSession(session, device.io(), 0, true) == Result::Invalid, "channel binding requires a fresh nonzero sequence seed");
    expect(bindSession(session, device.io(), 1, true) == Result::Ok, "bind exclusively owned packet transport");
    expect(bindSession(session, device.io(), 1, true) == Result::NotReady, "live binding cannot be reset and lose an outstanding sequence");
    expect(transact(session, Set::Active, kGraphicsParent, &rate, 1, 0, 1) == Result::Invalid && device.sends == 0,
           "zero poll budget performs no transport operations");
    expect(transact(session, Set::Active, kGraphicsParent, &rate, 1, 1, 1) == Result::Invalid && device.sends == 0,
           "budget must allow both publication and completion before a vote can be sent");
    expect(transact(session, Set::Active, kGraphicsParent, &rate, 1, 100000, 1000) == Result::Invalid,
           "combined wait bound prevents oversized wall-time budgets");
    device.responses = {ack(88), ack(1)};
    expect(transact(session, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::Ok,
           "wait through unrelated ACK and accept only the request's own ID");
    expect(session.staleAcks == 1 && session.state == State::Ready && device.sends == 1 && session.polls == 3,
           "stale ACK consumes bounded budget without resubmitting the resource write");
    expect(word(device.sent.data() + 36) == 27000, "lowest referenced GPU OPP is encoded in kHz");
    device.responses = {ack(2, "resource does not exist")};
    expect(transact(session, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::RemoteError &&
           session.state == State::Poisoned, "failed resource ACK cannot authorize subsequent hardware transitions");
    const unsigned sentBefore = device.sends;
    expect(transact(session, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::NotReady && device.sends == sentBefore,
           "poisoned session never retries an uncertain power operation");

    FakeTransport silent;
    Session timed = {};
    bindSession(timed, silent.io(), 10, true);
    expect(transact(timed, Set::Active, kGraphicsParent, &rate, 1, 7, 2) == Result::TimedOut,
           "lost ACK has a finite poll deadline");
    expect(timed.state == State::Poisoned && silent.sends == 1 && silent.receives == 6 &&
           silent.delays == 6 && silent.delayTotal == 12, "timeout does not imply the published vote was cancelled");
    silent.responses = {ack(10)};
    expect(transact(timed, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::NotReady,
           "late ACK cannot revive a timed-out resource transaction");

    FakeTransport full;
    full.blockedSends = 4;
    Session unsent = {};
    bindSession(unsent, full.io(), 1, true);
    expect(transact(unsent, Set::Active, kGraphicsParent, &rate, 1, 4, 1) == Result::TimedOut &&
           unsent.state == State::Ready && full.sent.empty(), "unpublished backpressure timeout has no RPM resource side effect");
    full.responses = {ack(2)};
    expect(transact(unsent, Set::Active, kGraphicsParent, &rate, 1, 4, 1) == Result::Ok,
           "a later unsent retry uses a distinct sequence number");

    for (bool failSend : {false, true}) {
        FakeTransport failed;
        Session broken = {};
        bindSession(broken, failed.io(), 1, true);
        if (failSend) { failed.sendStatus = Transfer::IoError; }
        else { failed.receiveStatus = Transfer::Closed; }
        expect(transact(broken, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::IoError &&
               broken.state == State::Poisoned, "I/O failure or peer reset poisons the resource session");
    }
    FakeTransport invalid;
    invalid.responses = {ack(1)};
    invalid.oversizedResponse = true;
    Session malformed = {};
    bindSession(malformed, invalid.io(), 1, true);
    expect(transact(malformed, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::ProtocolError,
           "transport's oversized response length is refused before parsing");
    FakeTransport finalId;
    finalId.responses = {ack(0xffffffff)};
    Session exhausted = {};
    bindSession(exhausted, finalId.io(), 0xffffffff, true);
    expect(transact(exhausted, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::Ok,
           "last nonzero message ID can complete normally");
    expect(transact(exhausted, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::MessageIdExhausted &&
           finalId.sends == 1, "sequence wrap cannot reuse an old message ID");
}

struct FakeSmd {
    std::array<u32, 22> control = {};
    std::array<u32, 128> transmit = {}, receive = {};
    struct Event { char kind; SmdSpace space; u32 offset; u32 value; };
    std::vector<Event> events;
    int failWriteAfter = -1;
    bool failNotify = false, failBarrier = false, emulateRpm = false, acknowledgeOpen = false;
    unsigned notifications = 0, violations = 0;
    unsigned delays = 0;
    u32 handledRequests = 0;
    SmdPipe *openingPipe = nullptr;

    FakeSmd() { control[0] = control[11] = 2; control[1] = control[12] = 1; }
    std::array<u32, 128> &fifo(SmdSpace space) { return space == SmdSpace::Transmit ? transmit : receive; }
    static bool read(void *context, SmdSpace space, u32 offset, u32 *value)
    {
        auto &device = *static_cast<FakeSmd *>(context);
        const u32 bound = space == SmdSpace::Control ? 88 : 512;
        if ((offset & 3) || offset >= bound) { ++device.violations; return false; }
        *value = space == SmdSpace::Control ? device.control[offset / 4] : device.fifo(space)[offset / 4];
        return true;
    }
    static bool write(void *context, SmdSpace space, u32 offset, u32 value)
    {
        auto &device = *static_cast<FakeSmd *>(context);
        if (device.failWriteAfter == 0) { return false; }
        if (device.failWriteAfter > 0) { --device.failWriteAfter; }
        const u32 bound = space == SmdSpace::Control ? 88 : 512;
        if ((offset & 3) || offset >= bound) { ++device.violations; return false; }
        if (space == SmdSpace::Control) { device.control[offset / 4] = value; }
        else { device.fifo(space)[offset / 4] = value; }
        device.events.push_back({'W', space, offset, value});
        return true;
    }
    static bool barrier(void *context)
    {
        auto &device = *static_cast<FakeSmd *>(context);
        device.events.push_back({'B', SmdSpace::Control, 0, 0});
        return !device.failBarrier;
    }
    static bool notify(void *context)
    {
        auto &device = *static_cast<FakeSmd *>(context);
        ++device.notifications;
        device.events.push_back({'N', SmdSpace::Control, 0, 0});
        if (device.failNotify) { return false; }
        if (device.openingPipe && device.control[0] == 1) {
            const u8 premature[4] = {};
            expect(device.openingPipe->state == PipeState::Opening &&
                   sendPacket(*device.openingPipe, premature, sizeof(premature)) == Transfer::Closed,
                   "resource traffic cannot enter an in-progress channel handshake");
        }
        if (device.acknowledgeOpen && device.control[0] == 1) {
            device.control[11] = 2;
            device.control[12] = 1;
            device.control[9] = device.control[21] = 0;
        }
        if (device.emulateRpm && device.control[9] != device.control[10]) {
            const u32 begin = device.control[9];
            const u32 packetBytes = device.transmit[begin / 4];
            expect(packetBytes == 40, "simulated remote receives exact request packet size");
            const u32 requestStart = (begin + 20) & 511;
            expect(device.transmit[requestStart / 4] == 0x00716572, "simulated RPM sees request service after SMD header");
            const u32 messageId = device.transmit[((requestStart + 8) & 511) / 4];
            device.control[9] = device.control[10];
            device.enqueue(ack(messageId), device.control[21], false);
            ++device.handledRequests;
        }
        return true;
    }
    static void delay(void *context, u32) { ++static_cast<FakeSmd *>(context)->delays; }
    SmdIo io() { return {this, read, write, barrier, notify, delay}; }
    void enqueue(const std::vector<u8> &payload, u32 start = 0, bool initializeReader = true)
    {
        for (u32 offset = 0; offset < 20; offset += 4) { receive[((start + offset) & 511) / 4] = 0; }
        receive[start / 4] = static_cast<u32>(payload.size());
        for (u32 index = 0; index < payload.size(); index += 4) {
            receive[((start + 20 + index) & 511) / 4] = word(payload.data() + index);
        }
        if (initializeReader) { control[20] = start; }
        control[21] = (start + 20 + static_cast<u32>(payload.size())) & 511;
    }
};

void testPipe()
{
    FakeSmd device;
    SmdPipe pipe = {};
    expect(adoptPipe(pipe, device.io(), 88, 512, false, true) == Result::Unowned, "SMD refuses a firmware-owned channel");
    expect(adoptPipe(pipe, device.io(), 88, 512, true, false) == Result::Invalid, "cached SMEM mappings without a coherency implementation are refused");
    for (u32 bytes : {0u, 256u, 513u, 131072u}) {
        expect(adoptPipe(pipe, device.io(), 88, bytes, true, true) == Result::Invalid, "FIFO geometry must be bounded, power of two and large enough");
    }
    expect(adoptPipe(pipe, device.io(), 87, 512, true, true) == Result::Invalid, "both 44-byte word-access control halves are required");
    device.control[10] = 4;
    expect(adoptPipe(pipe, device.io(), 88, 512, true, true) == Result::NotReady, "existing transmit packet prevents adopting an unquiesced channel");
    device.control[10] = 0;
    device.control[21] = 4;
    expect(adoptPipe(pipe, device.io(), 88, 512, true, true) == Result::NotReady, "existing receive packet prevents stealing a firmware reply");
    device.control[21] = 0;
    device.control[9] = device.control[10] = 508;
    expect(adoptPipe(pipe, device.io(), 88, 512, true, true) == Result::Ok, "adopt empty, exclusively owned, open word-access channel");
    Packet packet = {};
    const Value rate = clockRate(27000000);
    encodeRequest(packet, 1, Set::Active, kGraphicsParent, &rate, 1);
    device.events.clear();
    expect(sendPacket(pipe, packet.bytes, packet.size) == Transfer::Ok, "SMD packet wraps across FIFO boundary");
    expect(device.transmit[127] == 40 && device.control[10] == 56, "wrapped header and final producer index match packet length");
    for (u32 index = 0; index < packet.size; index += 4) {
        expect(device.transmit[((508 + 20 + index) & 511) / 4] == word(packet.bytes + index), "wrapped payload preserves each request word");
    }
    bool published = false, fifoBarrier = false, payloadWritten = false;
    for (const auto &event : device.events) {
        if (event.kind == 'W' && event.space == SmdSpace::Transmit) {
            expect(!published, "no FIFO bytes change after producer publication");
            payloadWritten = true; fifoBarrier = false;
        }
        if (event.kind == 'B' && payloadWritten) { fifoBarrier = true; }
        if (event.kind == 'W' && event.space == SmdSpace::Control && event.offset == 40) {
            expect(fifoBarrier, "release barrier separates payload stores from producer index"); published = true;
        }
        if (event.kind == 'N') { expect(published, "remote notification follows complete packet publication"); }
    }
    expect(device.notifications == 1 && device.violations == 0, "one packet notification with no mapping violations");
    device.control[9] = 60;
    device.events.clear();
    expect(sendPacket(pipe, packet.bytes, packet.size) == Transfer::WouldBlock && device.events.size() == 2,
           "full TX FIFO does not overwrite unread bytes or publish any control change");

    const auto response = ack(1);
    device.enqueue(response, 500);
    device.control[21] = 4;
    std::array<u8, 254> output;
    output.fill(0xa5);
    u32 size = 999;
    expect(receivePacket(pipe, output.data() + 1, 252, &size) == Transfer::WouldBlock &&
           device.control[20] == 500 && size == 0, "partial wrapped SMD header is not consumed");
    device.control[21] = 12;
    expect(receivePacket(pipe, output.data() + 1, 252, &size) == Transfer::WouldBlock && device.control[20] == 500,
           "header alone never advances receive index before the complete payload arrives");
    device.control[21] = 28;
    expect(receivePacket(pipe, output.data() + 1, 252, &size) == Transfer::Ok && size == response.size() &&
           !std::memcmp(output.data() + 1, response.data(), response.size()), "wrapped ACK is copied exactly once");
    expect(device.control[20] == 28 && output.front() == 0xa5 && output[21] == 0xa5 && output.back() == 0xa5,
           "receive index advances by complete frame and output sentinels remain intact");

    for (u32 invalidIndex : {1u, 512u, 0xffffffffu}) {
        FakeSmd invalid;
        SmdPipe invalidPipe = {};
        adoptPipe(invalidPipe, invalid.io(), 88, 512, true, true);
        invalid.control[21] = invalidIndex;
        expect(receivePacket(invalidPipe, output.data(), output.size(), &size) == Transfer::Closed &&
               invalidPipe.state == PipeState::Poisoned && invalid.violations == 0,
               "invalid peer index poisons channel before any out-of-bounds FIFO access");
    }
    for (u32 invalidSize : {0u, 19u, 253u, 0xffffffffu}) {
        FakeSmd invalid;
        SmdPipe invalidPipe = {};
        adoptPipe(invalidPipe, invalid.io(), 88, 512, true, true);
        invalid.enqueue(response);
        invalid.receive[0] = invalidSize;
        expect(receivePacket(invalidPipe, output.data(), output.size(), &size) == Transfer::IoError &&
               invalid.control[20] == 0 && size == 0, "hostile frame size is refused before copying or consuming data");
    }
    FakeSmd partial;
    SmdPipe broken = {};
    adoptPipe(broken, partial.io(), 88, 512, true, true);
    partial.failWriteAfter = 3;
    expect(sendPacket(broken, packet.bytes, packet.size) == Transfer::IoError &&
           partial.control[10] == 0 && partial.notifications == 0, "partial FIFO write is never published");
    expect(sendPacket(broken, packet.bytes, packet.size) == Transfer::Closed, "partially failed FIFO cannot be automatically reused");
    FakeSmd notified;
    SmdPipe uncertain = {};
    adoptPipe(uncertain, notified.io(), 88, 512, true, true);
    notified.failNotify = true;
    expect(sendPacket(uncertain, packet.bytes, packet.size) == Transfer::IoError &&
           uncertain.state == PipeState::Poisoned && notified.control[10] == 60,
           "failed interrupt notification retains uncertainty after producer index became visible");
}

void testIntegrated()
{
    FakeSmd device;
    SmdPipe pipe = {};
    Session session = {};
    device.emulateRpm = true;
    expect(adoptPipe(pipe, device.io(), 88, 512, true, true) == Result::Ok &&
           bindSession(session, pipeTransport(pipe), 31, true) == Result::Ok,
           "native wire protocol composes with actual packet FIFO implementation");
    const Value rate = clockRate(300000000);
    for (u32 index = 0; index < 16; ++index) {
        expect(transact(session, Set::Active, kGraphicsParent, &rate, 1, 10, 1) == Result::Ok &&
               session.lastAck.messageId == 31 + index, "correlated request/reply traverses both wrapping SMEM FIFOs");
    }
    expect(device.handledRequests == 16 && device.violations == 0 && session.nextMessageId == 47 &&
           device.control[10] == 448 && device.control[20] == 128,
           "repeated resource operations preserve sequence and FIFO ownership without firmware calls");
}

void testOpening()
{
    FakeSmd device;
    SmdPipe pipe = {};
    expect(openPipe(pipe, device.io(), 88, 512, true, true, 10, 1) == Result::NotReady && device.notifications == 0,
           "native opener cannot steal an already-open firmware channel");
    device.control[0] = device.control[11] = device.control[1] = device.control[12] = 0;
    device.control[9] = device.control[10] = 20;
    device.control[20] = device.control[21] = 40;
    device.acknowledgeOpen = true;
    device.openingPipe = &pipe;
    expect(openPipe(pipe, device.io(), 88, 512, false, true, 10, 1) == Result::Unowned,
           "closed-channel ownership is still required before resetting any indices");
    expect(openPipe(pipe, device.io(), 88, 512, true, true, 0, 1) == Result::Invalid,
           "invalid open deadline performs no handshake writes");
    expect(openPipe(pipe, device.io(), 88, 512, true, true, 10, 1) == Result::Ok &&
           pipe.state == PipeState::Ready && device.control[0] == 2 && device.control[11] == 2,
           "closed post-firmware channel completes a native polling OPENING/OPENED handshake");
    expect(device.control[1] == 1 && device.control[2] == 1 && device.control[3] == 1 &&
           device.control[9] == 0 && device.control[10] == 0 && device.control[20] == 0 &&
           device.control[21] == 0 && device.notifications == 2 && device.violations == 0,
           "each endpoint resets only its own indices and publishes carrier state before resource traffic");
    device.emulateRpm = true;
    Session session = {};
    bindSession(session, pipeTransport(pipe), 1, true);
    const Value rate = clockRate(27000000);
    expect(transact(session, Set::Active, kGraphicsParent, &rate, 1, 5, 1) == Result::Ok,
           "freshly reopened native channel carries a correlated RPM vote without a firmware function pointer");

    FakeSmd silent;
    silent.control[0] = silent.control[11] = silent.control[1] = silent.control[12] = 0;
    SmdPipe timeout = {};
    expect(openPipe(timeout, silent.io(), 88, 512, true, true, 7, 1) == Result::TimedOut &&
           timeout.state == PipeState::Poisoned && silent.notifications == 1 && silent.delays == 6,
           "unresponsive RPM open handshake has a finite deadline and retains uncertain channel ownership");
    expect(openPipe(timeout, silent.io(), 88, 512, true, true, 7, 1) == Result::NotReady,
           "timed-out handshake cannot be blindly retried on the same channel");
    FakeSmd pending;
    pending.control[0] = pending.control[11] = pending.control[1] = pending.control[12] = 0;
    pending.control[21] = 4;
    SmdPipe occupied = {};
    expect(openPipe(occupied, pending.io(), 88, 512, true, true, 7, 1) == Result::NotReady &&
           pending.notifications == 0, "closed but unconsumed response is not discarded during native opening");
}

}

int main()
{
    testPackets(); testSessions(); testPipe(); testIntegrated(); testOpening();
    std::printf("Native RPM/SMD: %u checks, %u failures (simulated shared memory; no handset votes)\n", checks, failures);
    return failures ? 1 : 0;
}
