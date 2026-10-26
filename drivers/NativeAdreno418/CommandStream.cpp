// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "CommandStream.h"

#include "BoardConstants.h"

namespace adreno {
namespace pm4 {

// ---------------------------------------------------------------------------
// Microcode
// ---------------------------------------------------------------------------

UcodeTarget ucodeTarget(CpUcode which)
{
    if (which == CpUcode::Pfp) {
        return UcodeTarget{ lumia950::kRegCpPfpUcodeAddr, lumia950::kRegCpPfpUcodeData };
    }
    return UcodeTarget{ lumia950::kRegCpMeRamWaddr, lumia950::kRegCpMeRamData };
}

bool planUcodeLoad(const u32 *blob, u32 blobWords, CpUcode which, UcodePlan *plan)
{
    if (!plan) { return false; }
    *plan = {};
    if (!blob) { return false; }
    const u32 versionIndex = which == CpUcode::Pfp ? 5 : 1;
    if (blobWords <= versionIndex || blobWords > 65536 ||
        (which == CpUcode::Pm4 && (blobWords - 1) % 3 != 0)) { return false; }
    if (blob[versionIndex] == 0xffffffffu || blob[versionIndex] == 0) { return false; }

    const UcodeTarget target = ucodeTarget(which);
    plan->addressIndex = target.addressIndex;
    plan->dataIndex = target.dataIndex;
    plan->data = blob + 1;
    plan->dataWords = blobWords - 1;
    plan->version = blob[versionIndex];
    return true;
}

u32 ucodeWriteCount(const UcodePlan &plan)
{
    // The address write, then one write per word through the data port.
    return plan.data ? 1 + plan.dataWords : 0;
}

bool ringBaseUsable(u32 address, u32 ringBytes)
{
    if (ringBytes == 0 || (ringBytes & (ringBytes - 1)) != 0) { return false; }
    // An address with no device behind it is not a ring the CP can walk, and the
    // alignment rule is the ring's own size, not a fixed 4 KiB.
    if (address == 0 || address == 0xffffffffu) { return false; }
    return (address & (ringBytes - 1)) == 0;
}

u32 header(u32 opcode, u32 payloadDwords)
{
    if (payloadDwords == 0 || payloadDwords > kMaxPayloadDwords) { return 0; }
    if (opcode > 0xffu) { return 0; }
    return kType3 | ((payloadDwords - 1) << kCountShift) | (opcode << 8);
}

u32 packetDwords(u32 payloadDwords)
{
    return payloadDwords == 0 || payloadDwords > kMaxPayloadDwords ? 0 : 1 + payloadDwords;
}

u32 writeNop(u32 *out, u32 capacity, u32 payloadDwords)
{
    if (!out || capacity < 1) { return 0; }
    const u32 words = packetDwords(payloadDwords);
    if (words == 0 || words > capacity) { return 0; }
    out[0] = header(kOpNop, payloadDwords);
    for (u32 index = 1; index < words; index++) { out[index] = 0; }
    return words;
}

u32 writeWaitForIdle(u32 *out, u32 capacity)
{
    if (!out || capacity < 2) { return 0; }
    out[0] = header(kOpWaitForIdle, 1);
    out[1] = 0;
    return 2;
}

u32 writeCacheFlush(u32 *out, u32 capacity, u32 event, bool interrupt)
{
    if (!out || capacity < 2) { return 0; }
    out[0] = header(kOpEventWrite, 1);
    out[1] = event | (interrupt ? kEventIrq : 0u);
    return 2;
}

u32 writeFence(u32 *out, u32 capacity, u32 address, u32 value, bool interrupt)
{
    if (!out || capacity < 4 || (address & 3) != 0) { return 0; }
    out[0] = header(kOpEventWrite, 3);
    out[1] = kEventCacheFlushTs | (interrupt ? kEventIrq : 0u);
    out[2] = address;
    out[3] = value;
    return 4;
}

u32 writeSubmissionTail(u32 *out, u32 capacity, u32 fenceAddress, u32 fenceValue,
                        bool interrupt)
{
    if (!out || (fenceAddress & 3) != 0) { return 0; }
    // 2 words for the flush, 2 for the wait, 4 for the fence.
    constexpr u32 kTailWords = 2 + 2 + 4;
    if (capacity < kTailWords) { return 0; }

    const u32 flush = writeCacheFlush(out, capacity, kEventHlsqFlush, false);
    const u32 wait = writeWaitForIdle(out + flush, capacity - flush);
    const u32 fence = writeFence(out + flush + wait, capacity - flush - wait,
                                 fenceAddress, fenceValue, interrupt);
    // All three fit or none was written: a tail with the fence missing would
    // leave a submission no one can wait for.
    if (flush == 0 || wait == 0 || fence == 0) { return 0; }
    return flush + wait + fence;
}

u32 writeMemoryFill(u32 *out, u32 capacity, u32 address, u32 value, u32 dwords)
{
    if (!out || dwords == 0 || dwords >= kMaxPayloadDwords ||
        (address & 3) != 0 || static_cast<u64>(address) + static_cast<u64>(dwords) * 4 >
        (1ull << 32) || capacity < dwords + 2) { return 0; }
    out[0] = header(kOpMemWrite, dwords + 1);
    out[1] = address;
    for (u32 index = 0; index < dwords; index++) { out[index + 2] = value; }
    return dwords + 2;
}

u32 writeMeInit(u32 *out, u32 capacity)
{
    if (!out || capacity < 18) { return 0; }
    const u32 values[17] = {0x3f7, 0, 0, 0, 0x80, 0x100, 0x180, 0x6600,
                           0x150, 0x14e, 0x154, 1, 0, 0, 0x20000000, 0, 0};
    out[0] = header(kOpMeInit, 17);
    for (u32 index = 0; index < 17; index++) { out[index + 1] = values[index]; }
    return 18;
}

u32 ringControl(u32 ringBytes)
{
    if (ringBytes < 4096 || ringBytes > 1024 * 1024 ||
        (ringBytes & (ringBytes - 1)) != 0) { return 0; }
    u32 quadwords = ringBytes / 8;
    u32 exponent = 0;
    while (quadwords > 1) { quadwords >>= 1; exponent++; }
    return (1u << 27) | exponent;
}

bool ringHasRoom(u32 writePointer, u32 readPointer, u32 ringDwords, u32 submissionDwords)
{
    if (ringDwords == 0 || submissionDwords == 0) { return false; }
    if (submissionDwords >= ringDwords) { return false; }
    // The mask below is the ring arithmetic; it is only the bound when the ring
    // is a power of two, which the CP requires and a caller could still get wrong.
    if ((ringDwords & (ringDwords - 1)) != 0) { return false; }
    // The pointers are dword indices; a stale one is masked into the ring first,
    // exactly as the ring arithmetic in AdrenoCore does.
    const u32 write = writePointer & (ringDwords - 1);
    const u32 read = readPointer & (ringDwords - 1);
    const u32 used = (write - read) & (ringDwords - 1);
    // One word of slack: a ring whose pointers meet is indistinguishable from an
    // empty one, so a submission never fills it completely.
    const u32 free = ringDwords - used - 1;
    return submissionDwords <= free;
}

u32 ringWritePointerAfter(u32 writePointer, u32 submissionDwords, u32 ringDwords)
{
    if (ringDwords == 0 || (ringDwords & (ringDwords - 1)) != 0) { return 0; }
    return (writePointer + submissionDwords) & (ringDwords - 1);
}

bool fenceReached(u32 last, u32 target)
{
    // Signed difference: a fence at 0xfffffff0 is reached by a counter that
    // wrapped through zero to 0x00000004.
    return static_cast<int>(last - target) >= 0;
}

void fenceWaitBegin(FenceWaiter *waiter, u32 target)
{
    if (!waiter) { return; }
    waiter->target = target;
    waiter->polls = 0;
}

FenceWait fenceWaitPoll(FenceWaiter *waiter, u32 sample)
{
    if (!waiter) { return FenceWait::TimedOut; }
    if (waiter->polls >= kFencePolls) { return FenceWait::TimedOut; }
    if (fenceReached(sample, waiter->target)) {
        waiter->polls = 0;
        return FenceWait::Complete;
    }
    waiter->polls++;
    // The budget is spent after exactly kFencePolls reads that did not see the
    // value, and a timed-out waiter stays timed out until it is begun again.
    return waiter->polls >= kFencePolls ? FenceWait::TimedOut : FenceWait::Pending;
}

} // namespace pm4
} // namespace adreno
