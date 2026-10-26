#ifndef NATIVE_ADRENO418_RPM_CORE_H
#define NATIVE_ADRENO418_RPM_CORE_H

#include "AdrenoTypes.h"

namespace adreno {
namespace rpm {

constexpr u32 kMaximumPacketBytes = 252;
constexpr u32 kMaximumValues = 16;
constexpr u32 kRequestService = 0x00716572;
constexpr u32 kMessageIdKey = 0x2367736d;
constexpr u32 kErrorKey = 0x00727265;
constexpr u32 kRateKey = 0x007a484b;

enum class Set : u32 { Active = 0, Sleep = 1 };
struct Resource { u32 type; u32 id; };
struct Value { u32 key; u32 value; };
struct Packet { u8 bytes[kMaximumPacketBytes]; u32 size; };

constexpr Resource kGraphicsParent = {0x326b6c63, 1};
constexpr Resource kMmssAhbParent = {0x316b6c63, 3};

bool encodeRequest(Packet &packet, u32 messageId, Set set, Resource resource,
                   const Value *values, u32 valueCount);
Value clockRate(u32 rateHz);

enum class AckKind : u32 { Accepted, RemoteError, Stale, Malformed };
struct Ack {
    AckKind kind;
    u32 messageId;
    char error[64];
};
Ack decodeAck(const u8 *bytes, u32 size, u32 expectedMessageId);

enum class Transfer : u32 { Ok, WouldBlock, IoError, Closed };
struct Transport {
    void *context;
    Transfer (*send)(void *, const u8 *, u32);
    Transfer (*receive)(void *, u8 *, u32 capacity, u32 *size);
    void (*delayUs)(void *, u32);
};

enum class State : u32 { Empty, Ready, InFlight, Poisoned };
enum class Result : u32 {
    Ok, Invalid, Unowned, NotReady, IoError, TimedOut, ProtocolError,
    RemoteError, MessageIdExhausted
};
struct Session {
    Transport transport;
    State state;
    u32 nextMessageId;
    u32 pendingMessageId;
    u32 polls;
    u32 staleAcks;
    Ack lastAck;
};

Result bindSession(Session &session, const Transport &transport,
                   u32 firstMessageId, bool exclusiveOwnership);
Result transact(Session &session, Set set, Resource resource,
                const Value *values, u32 valueCount, u32 pollBudget, u32 delayUs);

}
}

#endif
