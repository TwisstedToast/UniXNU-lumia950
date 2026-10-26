// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The command processor's stream (rungs 4 and 5, core only).
//
// The numbers and the packet shapes below are the ones Linux's msm driver writes for
// this exact GPU (v5.15, drivers/gpu/drm/msm/adreno/a4xx_gpu.c) over the
// generated table in adreno_pm4.xml.h:
//
//   CP_TYPE3_PKT     0xc0000000       CP_NOP             16
//   CP_WAIT_FOR_IDLE 38               CP_EVENT_WRITE     70
//   CACHE_FLUSH_TS   4 (event)        CACHE_FLUSH        6 (event)
//   HLSQ_FLUSH       7 (event)
//
// a4xx_gpu.c submits in this order, and it is the shape a fence has to follow:
//
//   CP_EVENT_WRITE  HLSQ_FLUSH
//   CP_WAIT_FOR_IDLE  0x00000000            (one payload word, always zero)
//   CP_EVENT_WRITE  CACHE_FLUSH_TS | BIT(31), fenceAddress, value
//
// BIT(31) in the event word raises CACHE_FLUSH_TS' interrupt; without it the
// packet still writes the value. The third form is the fence: a dword in memory
// that the CPU polls, and it is the only proof a submission finished.
//
// Nothing here allocates or submits. The caller owns the ring and the memory a
// fence points at; this file only builds the words and does the arithmetic.
#ifndef NATIVE_ADRENO418_COMMAND_STREAM_H
#define NATIVE_ADRENO418_COMMAND_STREAM_H

#include "AdrenoTypes.h"

namespace adreno {
namespace pm4 {

constexpr u32 kType3 = 0xc0000000u;
constexpr u32 kCountShift = 16;
constexpr u32 kMaxPayloadDwords = 0x4000u;

constexpr u32 kOpNop = 16;
constexpr u32 kOpWaitForIdle = 38;
constexpr u32 kOpEventWrite = 70;
constexpr u32 kOpMemWrite = 61;
constexpr u32 kOpMeInit = 72;

constexpr u32 kEventCacheFlushTs = 4;
constexpr u32 kEventCacheFlush = 6;
constexpr u32 kEventHlsqFlush = 7;

// BIT(31) of the event word asks the CP to raise the event's interrupt as well.
constexpr u32 kEventIrq = 1u << 31;

// A packet header: type, payload count, opcode. The count is the number of
// 32-bit words that follow the header, not the packet length.
u32 header(u32 opcode, u32 payloadDwords);

// Header plus payload.
u32 packetDwords(u32 payloadDwords);

// Each builder writes a whole packet into `out` and returns the words written,
// or 0 when the packet does not fit in `capacity` and nothing was written.
u32 writeNop(u32 *out, u32 capacity, u32 payloadDwords);
u32 writeWaitForIdle(u32 *out, u32 capacity);
u32 writeCacheFlush(u32 *out, u32 capacity, u32 event, bool interrupt);
u32 writeFence(u32 *out, u32 capacity, u32 address, u32 value, bool interrupt);

// The submission sequence a4xx_gpu.c uses, with the fence last: the flush, the
// wait, then the fence write. Returns the words written, or 0 if it does not fit.
u32 writeSubmissionTail(u32 *out, u32 capacity, u32 fenceAddress, u32 fenceValue,
                        bool interrupt);

u32 writeMemoryFill(u32 *out, u32 capacity, u32 address, u32 value, u32 dwords);
u32 writeMeInit(u32 *out, u32 capacity);
u32 ringControl(u32 ringBytes);

// Ring pacing. Both pointers are dword indices into a ring of `ringDwords`; the
// write pointer moves by whole packets, and a submission that does not fit is
// refused rather than wrapped over work the CP has not read.
bool ringHasRoom(u32 writePointer, u32 readPointer, u32 ringDwords, u32 submissionDwords);
u32 ringWritePointerAfter(u32 writePointer, u32 submissionDwords, u32 ringDwords);

// ---------------------------------------------------------------------------
// Getting a command processor at all (the gate on rung 4)
//
// A ring is not enough to submit work: the a4xx CP runs microcode, and Linux's
// a4xx_gpu.c loads two blobs into it before it programs a ring — PM4 into
// CP_ME_RAM and the PFP into CP_PFP_UCODE — then writes CP_ME_CNTL = 0 to clear
// ME_HALT and start it.
// ---------------------------------------------------------------------------

enum class CpUcode {
    Pm4,  // the micro engine's command stream interpreter
    Pfp,  // the prefetch parser
};

struct UcodeTarget {
    u32 addressIndex;  // dword index of the address register
    u32 dataIndex;     // dword index of the data port
};

UcodeTarget ucodeTarget(CpUcode which);

struct UcodePlan {
    u32 addressIndex;
    u32 dataIndex;
    const u32 *data;
    u32 dataWords;
    u32 version;
};

bool planUcodeLoad(const u32 *blob, u32 blobWords, CpUcode which, UcodePlan *plan);

// Register writes the plan costs: one address, then one per image word.
u32 ucodeWriteCount(const UcodePlan &plan);

// CP_RB_BASE holds the ring's address, which the CP requires aligned to the
// ring's own size, and the size has to be a power of two.
bool ringBaseUsable(u32 address, u32 ringBytes);

// A fence is reached when the word the CP wrote has caught up with the value the
// submission asked for. Counters wrap, so the comparison is signed rather than
// `>=`: `last` is reached when the 32-bit difference from `target` is
// non-negative.
bool fenceReached(u32 last, u32 target);

// How many polls a caller may spend waiting for a fence before it calls it lost.
// A submission that never completes has to become an error the caller can
// report, not a wait that ends the thread's turn on the CPU forever: on a phone
// with no console, a hang here is indistinguishable from a dead board.
constexpr u32 kFencePolls = 1000;

// The wait, as a state machine, so it is testable without a GPU: the caller
// reads the fence word and hands the sample in.
enum class FenceWait {
    Pending,   // the CP has not written the value yet; read again
    Complete,  // it has
    TimedOut,  // the poll budget is spent; the submission is lost
};

struct FenceWaiter {
    u32 target;
    u32 polls;
};

void fenceWaitBegin(FenceWaiter *waiter, u32 target);
FenceWait fenceWaitPoll(FenceWaiter *waiter, u32 sample);

} // namespace pm4
} // namespace adreno

#endif // NATIVE_ADRENO418_COMMAND_STREAM_H
