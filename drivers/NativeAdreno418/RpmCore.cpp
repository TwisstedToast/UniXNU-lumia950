#include "RpmCore.h"

namespace adreno {
namespace rpm {
namespace {

u32 read32(const u8 *bytes)
{
    return bytes[0] | (static_cast<u32>(bytes[1]) << 8) |
        (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
}

void write32(u8 *bytes, u32 value)
{
    for (u32 index = 0; index < 4; ++index) {
        bytes[index] = static_cast<u8>(value >> (index * 8));
    }
}

Result poison(Session &session, Result result)
{
    session.state = State::Poisoned;
    return result;
}

}

bool encodeRequest(Packet &packet, u32 messageId, Set set, Resource resource,
                   const Value *values, u32 valueCount)
{
    packet = {};
    if (!messageId || (set != Set::Active && set != Set::Sleep) ||
        !resource.type || !values || !valueCount || valueCount > kMaximumValues) {
        return false;
    }
    for (u32 index = 0; index < valueCount; ++index) {
        if (!values[index].key) { return false; }
        for (u32 previous = 0; previous < index; ++previous) {
            if (values[index].key == values[previous].key) { return false; }
        }
    }
    const u32 dataBytes = valueCount * 12;
    const u32 header[] = {kRequestService, 20 + dataBytes, messageId,
                          static_cast<u32>(set), resource.type, resource.id, dataBytes};
    for (u32 index = 0; index < 7; ++index) { write32(packet.bytes + index * 4, header[index]); }
    for (u32 index = 0; index < valueCount; ++index) {
        u8 *destination = packet.bytes + 28 + index * 12;
        write32(destination, values[index].key);
        write32(destination + 4, 4);
        write32(destination + 8, values[index].value);
    }
    packet.size = 28 + dataBytes;
    return true;
}

Value clockRate(u32 rateHz)
{
    return {kRateKey, static_cast<u32>((static_cast<u64>(rateHz) + 999) / 1000)};
}

Ack decodeAck(const u8 *bytes, u32 size, u32 expectedMessageId)
{
    Ack result = {};
    result.kind = AckKind::Malformed;
    if (!bytes || !expectedMessageId || size < 20 || size > kMaximumPacketBytes ||
        (size & 3) || read32(bytes) != kRequestService || read32(bytes + 4) != size - 8) {
        return result;
    }
    bool foundId = false;
    bool foundError = false;
    for (u32 offset = 8; offset < size;) {
        if (size - offset < 8) { return result; }
        const u32 key = read32(bytes + offset);
        const u32 length = read32(bytes + offset + 4);
        if (length > size - offset - 8) { return result; }
        const u32 padded = (length + 3) & ~3u;
        if (padded > size - offset - 8) { return result; }
        const u8 *payload = bytes + offset + 8;
        if (key == kMessageIdKey) {
            if (foundId || length != 4) { return result; }
            result.messageId = read32(payload);
            foundId = true;
        } else if (key == kErrorKey) {
            if (foundError || !length) { return result; }
            foundError = true;
            const u32 limit = length < sizeof(result.error) - 1 ? length : sizeof(result.error) - 1;
            for (u32 index = 0; index < limit; ++index) {
                const u8 value = payload[index];
                if (!value) { break; }
                result.error[index] = value >= 32 && value <= 126 ? static_cast<char>(value) : '?';
            }
        } else { return result; }
        offset += 8 + padded;
    }
    if (!foundId || !result.messageId) { return result; }
    result.kind = result.messageId != expectedMessageId ? AckKind::Stale :
        foundError ? AckKind::RemoteError : AckKind::Accepted;
    return result;
}

Result bindSession(Session &session, const Transport &transport,
                   u32 firstMessageId, bool exclusiveOwnership)
{
    if (session.state != State::Empty) { return Result::NotReady; }
    if (!exclusiveOwnership) { return Result::Unowned; }
    if (!firstMessageId || !transport.send || !transport.receive || !transport.delayUs) {
        return Result::Invalid;
    }
    session = {};
    session.transport = transport;
    session.nextMessageId = firstMessageId;
    session.state = State::Ready;
    return Result::Ok;
}

Result transact(Session &session, Set set, Resource resource,
                const Value *values, u32 valueCount, u32 pollBudget, u32 delayUs)
{
    if (session.state != State::Ready) { return Result::NotReady; }
    if (pollBudget < 2 || pollBudget > 100000 || delayUs > 1000 ||
        static_cast<u64>(pollBudget) * delayUs > 5000000) { return Result::Invalid; }
    if (!session.nextMessageId) { return Result::MessageIdExhausted; }
    Packet request = {};
    if (!encodeRequest(request, session.nextMessageId, set, resource, values, valueCount)) {
        return Result::Invalid;
    }
    session.pendingMessageId = session.nextMessageId++;
    session.polls = 0;
    session.staleAcks = 0;
    session.lastAck = {};
    session.lastAck.kind = AckKind::Malformed;
    session.state = State::InFlight;
    bool published = false;
    for (u32 attempt = 0; attempt < pollBudget; ++attempt) {
        ++session.polls;
        if (!published) {
            const Transfer sent = session.transport.send(session.transport.context,
                                                         request.bytes, request.size);
            if (sent == Transfer::Ok) { published = true; }
            else if (sent != Transfer::WouldBlock) { return poison(session, Result::IoError); }
        } else {
            Packet response = {};
            const Transfer received = session.transport.receive(session.transport.context,
                response.bytes, sizeof(response.bytes), &response.size);
            if (received == Transfer::Ok) {
                session.lastAck = decodeAck(response.bytes, response.size, session.pendingMessageId);
                if (session.lastAck.kind == AckKind::Accepted) {
                    session.pendingMessageId = 0;
                    session.state = State::Ready;
                    return Result::Ok;
                }
                if (session.lastAck.kind == AckKind::RemoteError) { return poison(session, Result::RemoteError); }
                if (session.lastAck.kind == AckKind::Malformed) { return poison(session, Result::ProtocolError); }
                ++session.staleAcks;
            } else if (received != Transfer::WouldBlock) { return poison(session, Result::IoError); }
        }
        if (attempt + 1 < pollBudget) { session.transport.delayUs(session.transport.context, delayUs); }
    }
    if (published) { return poison(session, Result::TimedOut); }
    session.state = State::Ready;
    session.pendingMessageId = 0;
    return Result::TimedOut;
}

}
}
