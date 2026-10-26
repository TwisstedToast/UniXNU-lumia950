// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Host test for AdrenoCore. Build and run on the development machine:
//
//   g++ -std=c++17 -Wall -Wextra -Werror -I drivers/NativeAdreno418
//       drivers/NativeAdreno418/Test/check-adreno-core.cpp
//       drivers/NativeAdreno418/AdrenoCore.cpp -o build/check-adreno-core
//   ./build/check-adreno-core
//
// What this pins is the arithmetic and the refusals, not any hardware: the
// register offsets are the register database's dword indices, the GDSCR bit is
// the one drivers/clk/qcom/gdsc.c polls, and the ring rules are the ones a
// circular dword ring has regardless of which CP walks it.
#include <cstdio>
#include <cstring>

#include "../AdrenoCore.h"
#include "../BoardConstants.h"
#include "../CommandStream.h"
#include "../IommuCore.h"

using adreno::u32;
using adreno::u64;

static int failures = 0;

static void expect(bool condition, const char *what)
{
    if (condition) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static void expectEq(u64 actual, u64 expected, const char *what)
{
    if (actual == expected) {
        std::printf("ok   %s (%llu)\n", what, static_cast<unsigned long long>(actual));
    } else {
        std::printf("FAIL %s: got %llu, expected %llu\n", what,
                    static_cast<unsigned long long>(actual),
                    static_cast<unsigned long long>(expected));
        ++failures;
    }
}

int main()
{
    // --- the device tree's chipid ---------------------------------------
    const adreno::ChipIdentity identity = adreno::decodeChipId(lumia950::kChipId);
    expectEq(identity.generation, 4, "0x04010800 is generation 4 (a4xx)");
    expectEq(identity.major, 1, "0x04010800 major is 1");
    expectEq(identity.minor, 8, "0x04010800 minor is 8");
    expectEq(identity.patch, 0, "0x04010800 patch is 0");
    expect(adreno::isA4xx(identity), "the Lumia 950's part is the family this driver covers");

    const adreno::ChipIdentity a530 = adreno::decodeChipId(0x05030000u);
    expect(!adreno::isA4xx(a530), "an a5xx chipid is refused rather than treated as a4xx");
    expectEq(a530.generation, 5, "0x05030000 is generation 5");

    // --- the GDSC state --------------------------------------------------
    expect(adreno::decodeGdscr(0x80000000u) == adreno::PowerState::On,
           "PWR_ON alone means the domain is on");
    expect(adreno::decodeGdscr(0x00000000u) == adreno::PowerState::Collapsed,
           "a GDSCR without PWR_ON is collapsed");
    expect(adreno::decodeGdscr(0x80000003u) == adreno::PowerState::On,
           "PWR_ON wins over unrelated low bits");
    expect(adreno::decodeGdscr(0x7fffffffu) == adreno::PowerState::Collapsed,
           "every bit except PWR_ON is not enough");

    expect(adreno::gdscrCollapseRequested(0x80000001u), "SW_COLLAPSE is bit 0");
    expect(!adreno::gdscrCollapseRequested(0x80000000u), "a powered domain without the request is settled");
    expect(!adreno::gdscrCollapseRequested(0x80000002u), "bit 1 is HW_CONTROL, not SW_COLLAPSE");

    // --- what a register read is worth -----------------------------------
    expect(adreno::classifyWindowRead(0xffffffffu) == adreno::WindowAnswer::NoResponse,
           "all ones is no device behind the window");
    expect(adreno::classifyWindowRead(0x04010800u) == adreno::WindowAnswer::Answered,
           "any other value is an answer");
    expect(!adreno::plausibleVersion(0u), "a zero version proves nothing");
    expect(!adreno::plausibleVersion(0xffffffffu), "all ones is not a version");
    expect(adreno::plausibleVersion(0x04010001u), "a real revision is plausible");

    // --- register addressing ---------------------------------------------
    expectEq(adreno::byteOffset(lumia950::kRegRbbmHwVersion), 0x000, "RBBM_HW_VERSION is at byte 0");
    expectEq(adreno::byteOffset(lumia950::kRegRbbmStatus), 0x644, "RBBM_STATUS index 0x191 is byte 0x644");
    expectEq(adreno::byteOffset(lumia950::kRegCpRbBase), 0x800, "CP_RB_BASE index 0x200 is byte 0x800");
    expectEq(adreno::byteOffset(lumia950::kRegCpMeCntl), 0x8b4, "CP_ME_CNTL index 0x22d is byte 0x8b4");

    // --- the command ring -------------------------------------------------
    const u32 size = lumia950::kCpRingDwords;
    expectEq(size, 0x4000, "the ring this driver will use is a power of two");

    expectEq(adreno::ringUsedDwords(10, 4, size), 6, "ten written, four read is six queued");
    expectEq(adreno::ringUsedDwords(3, size - 1, size), 4, "a wrap measures the wrapped distance");
    expectEq(adreno::ringUsedDwords(7, 7, size), 0, "equal pointers means nothing queued");
    expectEq(adreno::ringFreeDwords(10, 4, size), size - 6, "free space is the rest of the ring");
    expectEq(adreno::ringFreeDwords(7, 7, size), size, "an idle ring is entirely free");

    expectEq(adreno::ringAdvance(size - 1, 3, size), 2, "advancing past the end wraps");
    expectEq(adreno::ringAdvance(0, size, size), 0, "a whole ring is no movement");
    expectEq(adreno::ringAdvance(size + 5, 0, size), 5, "a stale pointer outside the ring is masked");

    expect(adreno::ringPointerValid(0, size), "zero is inside the ring");
    expect(adreno::ringPointerValid(size - 1, size), "the last word is inside the ring");
    expect(!adreno::ringPointerValid(size, size), "the ring size itself is outside");
    expect(!adreno::ringPointerValid(size + 1, size), "a pointer past the end is refused");

    expectEq(adreno::ringUsedDwords(10, 4, 0), 0, "a zero-sized ring reports nothing queued");
    expectEq(adreno::ringFreeDwords(10, 4, 0), 0, "a zero-sized ring has no free space");
    expectEq(adreno::ringAdvance(5, 5, 0), 0, "a zero-sized ring cannot advance");
    expect(!adreno::ringPointerValid(0, 0), "a zero-sized ring holds no valid pointer");

    // --- the GDSC sequence ------------------------------------------------
    // Qualcomm's gdsc.c: refuse under hardware control, change SW_COLLAPSE and
    // nothing else, then poll PWR_ON for at most 100 reads.
    expectEq(adreno::kGdscrSwCollapse, 0x1, "SW_COLLAPSE is bit 0");
    expectEq(adreno::kGdscrHwControl, 0x2, "HW_CONTROL is bit 1");
    expectEq(adreno::gdscrEnableValue(0x80000003u), 0x80000002u,
             "enable clears SW_COLLAPSE and keeps every other bit");
    expectEq(adreno::gdscrDisableValue(0x80000002u), 0x80000003u,
             "disable sets SW_COLLAPSE and keeps every other bit");
    expectEq(adreno::gdscrEnableValue(0x00000001u), 0x00000000u,
             "enable never sets PWR_ON itself; the hardware does");
    expect(adreno::gdscrUnderHardwareControl(0x80000002u), "HW_CONTROL is refused");
    expect(!adreno::gdscrUnderHardwareControl(0x80000000u), "a settled software domain is not HW-controlled");

    adreno::GdscPoller poller;
    adreno::gdscPollBegin(&poller);
    u32 waits = 0;
    adreno::GdscPoll result = adreno::GdscPoll::Waiting;
    while (result == adreno::GdscPoll::Waiting && waits < 1000) {
        result = adreno::gdscPollUntilOn(&poller, 0x00000000u);
        if (result == adreno::GdscPoll::Waiting) { ++waits; }
    }
    expectEq(waits, adreno::kGdscrMaxPolls - 1, "a domain that never powers gets the full read budget");
    expect(result == adreno::GdscPoll::TimedOut, "and then times out");

    adreno::gdscPollBegin(&poller);
    expect(adreno::gdscPollUntilOn(&poller, 0x80000000u) == adreno::GdscPoll::PoweredOn,
           "PWR_ON ends the poll on the first read");
    adreno::gdscPollBegin(&poller);
    expect(adreno::gdscPollUntilOn(&poller, 0x00000001u) == adreno::GdscPoll::Waiting,
           "a collapse request with no PWR_ON keeps waiting");
    expect(adreno::gdscPollUntilOn(&poller, 0x80000001u) == adreno::GdscPoll::PoweredOn,
           "PWR_ON wins even while a collapse request is still set");

    adreno::gdscPollBegin(&poller);
    expect(adreno::gdscPollUntilOff(&poller, 0x00000000u) == adreno::GdscPoll::PoweredOff,
           "a collapsed domain ends the off poll");
    expect(adreno::gdscPollUntilOff(&poller, 0x80000003u) == adreno::GdscPoll::Waiting,
           "a still-powered domain keeps the off poll waiting");

    // --- the GPU's SMMU page tables ----------------------------------------
    expectEq(adreno::smmu::kFirstLevelEntries, 4096, "the first level covers 4096 MiB");
    expectEq(adreno::smmu::kSectionBytes, 0x100000, "one first-level entry is 1 MiB");
    expectEq(adreno::smmu::kTableBytes, 1024, "a coarse table is 1 KiB");
    expectEq(adreno::smmu::firstLevelIndex(0x80000000u), 0x800, "0x80000000 is entry 0x800");
    expectEq(adreno::smmu::firstLevelIndex(0x800fffffu), 0x800, "the last byte of the MiB is the same entry");
    expectEq(adreno::smmu::firstLevelIndex(0x80100000u), 0x801, "the next MiB is the next entry");
    expectEq(adreno::smmu::secondLevelIndex(0x80000000u), 0, "the first page is index 0");
    expectEq(adreno::smmu::secondLevelIndex(0x80001234u), 1, "0x80001234 is index 1");
    expectEq(adreno::smmu::secondLevelIndex(0x800ff000u), 255, "the last page is index 255");

    expectEq(adreno::smmu::attributeShift(1), 10, "a first-level attribute field starts at bit 10");
    expectEq(adreno::smmu::attributeShift(2), 4, "a second-level attribute field starts at bit 4");
    expectEq(adreno::smmu::executeNeverBit(1), 0x10, "XN is bit 4 in a section");
    expectEq(adreno::smmu::executeNeverBit(2), 0x1, "XN is bit 0 in a page");

    expectEq(adreno::smmu::pageDescriptor(0x80000000u, true, false, true), 0x80000c7eu,
             "a cacheable page descriptor");
    expectEq(adreno::smmu::pageDescriptor(0x80000000u, true, true, true), 0x80000c7fu,
             "execute-never only moves bit 0");
    expectEq(adreno::smmu::pageDescriptor(0x80000000u, false, false, true), 0x80000c72u,
             "a normal noncacheable page keeps TEX(1) and drops B and C");
    expectEq(adreno::smmu::sectionDescriptor(0x80000000u, true, false, true), 0x800b1c0eu,
             "a non-secure section descriptor");
    expectEq(adreno::smmu::sectionDescriptor(0x80000000u, true, false, false), 0x80031c0eu,
             "a secure section descriptor has no NS bit");
    expectEq(adreno::smmu::tableDescriptor(0x80000400u, false), 0x80000401u,
             "a coarse table pointer is table-type and 1 KiB aligned");
    expectEq(adreno::smmu::tableDescriptor(0x80000400u, true), 0x80000409u,
             "a non-secure table pointer sets the NS table bit");
    expectEq(adreno::smmu::pageDescriptor(0x100000000ull, true, false, true), 0u,
             "a 4 GiB output address is outside the format");
    expectEq(adreno::smmu::tableDescriptor(0x100000000ull, false), 0u,
             "a 4 GiB table address is outside the format");

    alignas(1024) static u32 firstLevel[adreno::smmu::kFirstLevelEntries];
    alignas(1024) static u32 tablesPool[2 * adreno::smmu::kSecondLevelEntries];
    for (u32 &entry : firstLevel) { entry = 0; }
    for (u32 &entry : tablesPool) { entry = 0; }

    adreno::smmu::Tables tables = {};
    tables.firstLevel = firstLevel;
    tables.secondLevel = tablesPool;
    tables.secondLevelPhysical = 0x90000000u;
    tables.secondLevelCount = 2;
    tables.secondLevelUsed = 0;
    tables.nonSecure = true;

    expect(!adreno::smmu::mapPage(tables, 0x1001, 0x2000, true, false),
           "an unaligned input address is refused");
    expect(!adreno::smmu::mapPage(tables, 0x80000000u, 0x2001, true, false),
           "an unaligned output address is refused");
    expect(!adreno::smmu::mapPage(tables, 0x100000000ull, 0x2000, true, false),
           "an input address above 4 GiB is refused");
    expectEq(tables.secondLevelUsed, 0, "a refused map allocates no table");

    expect(adreno::smmu::mapPage(tables, 0x80000000u, 0x40000000u, true, false),
           "mapping a page allocates its coarse table");
    expectEq(tables.secondLevelUsed, 1, "one table allocated");
    expectEq(firstLevel[0x800], 0x90000009u, "the first-level entry points at the pool, non-secure");
    expectEq(tablesPool[0], 0x40000c7eu, "the page descriptor lands in the second level");

    expect(adreno::smmu::mapPage(tables, 0x80001000u, 0x40001000u, true, false),
           "a second page in the same megabyte reuses the table");
    expectEq(tables.secondLevelUsed, 1, "and allocates no second table");
    expect(!adreno::smmu::mapPage(tables, 0x80001000u, 0x40002000u, true, false),
           "a page already mapped is refused rather than moved");
    expect(adreno::smmu::unmapPage(tables, 0x80001000u), "the page unmaps");
    expect(!adreno::smmu::unmapPage(tables, 0x80001000u), "unmapping it twice reports nothing mapped");
    expect(adreno::smmu::mapPage(tables, 0x80001000u, 0x40001000u, true, false),
           "and it maps again");

    expect(adreno::smmu::mapPage(tables, 0x80100000u, 0x40100000u, true, false),
           "a different megabyte allocates the second table");
    expectEq(tables.secondLevelUsed, 2, "both tables used");
    expect(!adreno::smmu::mapPage(tables, 0x80200000u, 0x40200000u, true, false),
           "an exhausted pool refuses the next megabyte");
    expectEq(tables.secondLevelUsed, 2, "and allocates nothing it cannot address");

    expect(adreno::smmu::mapSection(firstLevel, 0x80300000u, 0x40300000u, true, false, true),
           "a section maps at the first level");
    expectEq(firstLevel[0x803], 0x403b1c0eu, "the section descriptor is in place");
    expect(!adreno::smmu::mapPage(tables, 0x80300000u, 0x50000000u, true, false),
           "a page inside a mapped section is refused");
    expect(!adreno::smmu::mapSection(firstLevel, 0x80000000u, 0x60000000u, true, false, true),
           "a section over a page table is refused");
    expect(!adreno::smmu::mapSection(firstLevel, 0x80480000u, 0x60480000u, true, false, true),
           "an unaligned section is refused");

    expectEq(adreno::smmu::pagesFor(0x1000, 1), 1, "one byte in one page is one page");
    expectEq(adreno::smmu::pagesFor(0x1fff, 2), 2, "two bytes across a boundary are two pages");
    expectEq(adreno::smmu::pagesFor(0x1234, 0x1000), 2, "an unaligned start rounds down");
    expectEq(adreno::smmu::pagesFor(0x1000, 0), 0, "an empty range is no pages");

    // --- the command processor's stream (rung 4) ---------------------------
    expectEq(adreno::pm4::kType3, 0xc0000000, "command packets are PM4 type 3");
    expectEq(adreno::pm4::header(adreno::pm4::kOpNop, 1), 0xc0001000u, "a NOP header");
    expectEq(adreno::pm4::header(adreno::pm4::kOpNop, 0), 0u, "zero payload is invalid");
    expectEq(adreno::pm4::header(adreno::pm4::kOpEventWrite, 3), 0xc0024600u,
             "CP_EVENT_WRITE with three payload words");
    expectEq(adreno::pm4::header(adreno::pm4::kOpWaitForIdle, 1), 0xc0002600u,
             "CP_WAIT_FOR_IDLE with one payload word");
    expectEq(adreno::pm4::header(adreno::pm4::kOpNop, 0x4001), 0u,
             "a payload count past 14 bits is refused, not masked");
    expectEq(adreno::pm4::header(0x100, 0), 0u, "an opcode past 8 bits is refused");
    expectEq(adreno::pm4::packetDwords(3), 4, "a packet is its header plus its payload");
    expectEq(adreno::pm4::header(adreno::pm4::kOpNop, 0x4000), 0xffff1000u,
             "the largest packet uses count minus one in bits 29:16");
    expectEq(adreno::pm4::packetDwords(0), 0, "zero payload is not representable");
    expectEq(adreno::pm4::ringControl(65536), 0x0800000d, "ring size is log2 of quadwords with RPTR shadow disabled");
    expectEq(adreno::pm4::ringControl(4095), 0, "invalid ring byte count is refused");

    u32 stream[16];
    for (u32 &word : stream) { word = 0xdeadbeefu; }

    expectEq(adreno::pm4::writeWaitForIdle(stream, 16), 2, "wait-for-idle is two words");
    expectEq(stream[0], 0xc0002600u, "wait-for-idle header");
    expectEq(stream[1], 0, "wait-for-idle payload is zero");

    expectEq(adreno::pm4::writeCacheFlush(stream, 16, adreno::pm4::kEventHlsqFlush, false), 2,
             "a cache flush is two words");
    expectEq(stream[0], 0xc0004600u, "event-write header");
    expectEq(stream[1], 7, "HLSQ_FLUSH is event 7");

    expectEq(adreno::pm4::writeFence(stream, 16, 0x90001000u, 42, true), 4,
             "a fence is four words");
    expectEq(stream[0], 0xc0024600u, "fence header");
    expectEq(stream[1], 0x80000004u, "CACHE_FLUSH_TS with the interrupt bit");
    expectEq(stream[2], 0x90001000u, "the fence address");
    expectEq(stream[3], 42, "the value the CP writes");

    expectEq(adreno::pm4::writeFence(stream, 16, 0x90001000u, 43, false), 4,
             "a fence without the interrupt bit is still four words");
    expectEq(stream[1], 4, "and its event word is plain CACHE_FLUSH_TS");

    u32 untouched = 0x12345678u;
    expectEq(adreno::pm4::writeFence(&untouched, 3, 0, 0, false), 0,
             "a fence that does not fit writes nothing");
    expectEq(untouched, 0x12345678u, "and leaves the word alone");
    expectEq(adreno::pm4::writeNop(nullptr, 4, 0), 0, "no stream is no packet");

    for (u32 &word : stream) { word = 0u; }
    expectEq(adreno::pm4::writeSubmissionTail(stream, 16, 0x90001000u, 7, true), 8,
             "the submission tail is flush, wait, fence");
    expectEq(stream[0], 0xc0004600u, "tail word 0: event-write");
    expectEq(stream[1], 7, "tail word 1: HLSQ_FLUSH");
    expectEq(stream[2], 0xc0002600u, "tail word 2: wait-for-idle");
    expectEq(stream[3], 0, "tail word 3: its zero");
    expectEq(stream[4], 0xc0024600u, "tail word 4: the fence");
    expectEq(stream[5], 0x80000004u, "tail word 5: CACHE_FLUSH_TS with the IRQ");
    expectEq(stream[6], 0x90001000u, "tail word 6: the fence address");
    expectEq(stream[7], 7, "tail word 7: the value");
    expectEq(adreno::pm4::writeMemoryFill(stream, 16, 0x10001000, 0xabcd1234, 4), 6,
             "CP memory fill has address plus data payload");
    expectEq(stream[0], 0xc0043d00, "memory-write golden header from an independent packet layout");
    expectEq(stream[1], 0x10001000, "memory-write address");
    expectEq(stream[5], 0xabcd1234, "memory-write final data word");
    expectEq(adreno::pm4::writeMemoryFill(stream, 16, 0xfffffffcu, 0, 2), 0,
             "memory-write cannot wrap its 32-bit address space");
    expectEq(adreno::pm4::writeFence(stream, 16, 1, 1, false), 0, "unaligned fence refused");
    expectEq(adreno::pm4::writeSubmissionTail(stream, 16, 1, 1, false), 0,
             "invalid fence leaves the entire tail unwritten");

    const u32 ringDwords = lumia950::kCpRingDwords;
    expect(adreno::pm4::ringHasRoom(0, 0, ringDwords, 8), "an empty ring has room for a tail");
    expect(!adreno::pm4::ringHasRoom(ringDwords - 2, 0, ringDwords, 8),
           "an almost full ring refuses a tail that would wrap over it");
    expect(!adreno::pm4::ringHasRoom(0, 0, ringDwords, ringDwords),
           "a submission larger than the ring is refused");
    expectEq(adreno::pm4::ringWritePointerAfter(ringDwords - 4, 8, ringDwords), 4,
             "the write pointer wraps by the submission length");
    expect(!adreno::pm4::ringHasRoom(0, 0, 100, 8), "a ring that is not a power of two is refused");
    expectEq(adreno::pm4::ringWritePointerAfter(0, 8, 100), 0,
             "and so is a write pointer into one");
    for (u32 &word : stream) { word = 0u; }
    expectEq(adreno::pm4::writeSubmissionTail(stream, 7, 0x90001000u, 7, true), 0,
             "a submission tail that does not fit writes nothing");
    expectEq(stream[0], 0u, "and leaves the buffer alone");

    // --- microcode: the gate on rung 4 --------------------------------------
    expectEq(adreno::pm4::ucodeTarget(adreno::pm4::CpUcode::Pm4).addressIndex, 0x0225u,
             "PM4 goes through CP_ME_RAM_WADDR");
    expectEq(adreno::pm4::ucodeTarget(adreno::pm4::CpUcode::Pfp).addressIndex, 0x0223u,
             "the PFP through CP_PFP_UCODE_ADDR");
    expectEq(adreno::pm4::ucodeTarget(adreno::pm4::CpUcode::Pfp).dataIndex, 0x0224u,
             "and its data port");

    const u32 pm4Blob[] = { 0u, 0x004ff065u, 0x11111111u, 0x22222222u };
    adreno::pm4::UcodePlan plan = {};
    expect(adreno::pm4::planUcodeLoad(pm4Blob, 4, adreno::pm4::CpUcode::Pm4, &plan),
           "a blob with a version and an image is a plan");
    expectEq(plan.version, 0x004ff065u, "the version is image word 1, not its zero container header");
    expectEq(plan.dataWords, 3, "the image is everything after it");
    expectEq(plan.data[0], 0x004ff065u, "the first image word includes the version instruction");
    expectEq(adreno::pm4::ucodeWriteCount(plan), 4, "one address write and one per image word");

    const u32 versionOnly[] = { 0x00010203u };
    expect(!adreno::pm4::planUcodeLoad(versionOnly, 1, adreno::pm4::CpUcode::Pm4, &plan),
           "a version with no image is not a plan");
    expectEq(adreno::pm4::ucodeWriteCount(plan), 0, "a rejected replacement clears the previously valid load plan");
    const u32 deadWindow[] = { 0u, 0xffffffffu };
    expect(!adreno::pm4::planUcodeLoad(deadWindow, 2, adreno::pm4::CpUcode::Pfp, &plan),
           "an all-ones version is a dead window, not firmware");
    const u32 noVersion[] = { 0u, 0u };
    expect(!adreno::pm4::planUcodeLoad(noVersion, 2, adreno::pm4::CpUcode::Pm4, &plan),
           "zero is not a version a firmware image carries");
    adreno::pm4::UcodePlan fresh = {};
    expect(!adreno::pm4::planUcodeLoad(nullptr, 4, adreno::pm4::CpUcode::Pm4, &fresh),
           "no blob is no plan");
    expectEq(adreno::pm4::ucodeWriteCount(fresh), 0, "and a refused plan writes nothing");
    expect(!adreno::pm4::planUcodeLoad(pm4Blob, 4, adreno::pm4::CpUcode::Pm4, nullptr),
           "nowhere to put the plan is a refusal");
    expectEq(plan.dataWords, 0, "a failed load plan cannot retain an earlier firmware image");
    const u32 pfpBlob[] = {0, 1, 2, 3, 4, 0x4ff083};
    expect(adreno::pm4::planUcodeLoad(pfpBlob, 6, adreno::pm4::CpUcode::Pfp, &plan),
           "PFP with its container header and version instruction is accepted");
    expectEq(plan.version, 0x4ff083, "PFP version is word five");
    expect(!adreno::pm4::planUcodeLoad(nullptr, 6, adreno::pm4::CpUcode::Pfp, &plan),
           "a null firmware replacement is refused after a valid plan");
    expect(plan.data == nullptr && plan.dataWords == 0 && plan.version == 0 &&
           plan.addressIndex == 0 && plan.dataIndex == 0, "every stale load-plan field is cleared before input validation");
    expect(!adreno::pm4::planUcodeLoad(pfpBlob, 5, adreno::pm4::CpUcode::Pfp, &plan),
           "truncated PFP is refused before the version read");

    expect(adreno::pm4::ringBaseUsable(0x90000000u, 0x10000),
           "a ring base aligned to its own size is usable");
    expect(!adreno::pm4::ringBaseUsable(0x90000004u, 0x10000),
           "an unaligned base is not; the CP requires the ring's size as alignment");
    expect(!adreno::pm4::ringBaseUsable(0x90000000u, 0x18000),
           "a ring that is not a power of two has no alignment rule");
    expect(!adreno::pm4::ringBaseUsable(0xffffffffu, 0x10000), "all ones is not an address");
    expect(!adreno::pm4::ringBaseUsable(0u, 0x10000), "and neither is zero");

    // --- the fence (rung 5) -----------------------------------------------
    expect(adreno::pm4::fenceReached(5, 5), "an equal counter is reached");
    expect(!adreno::pm4::fenceReached(4, 5), "a counter behind the value is not");
    expect(adreno::pm4::fenceReached(6, 5), "a counter past the value is reached");
    expect(adreno::pm4::fenceReached(0x00000004u, 0xfffffff0u),
           "a counter that wrapped through zero reached the value");
    expect(!adreno::pm4::fenceReached(0xfffffff0u, 0x00000004u),
           "and the stale pre-wrap counter did not");
    expectEq(adreno::pm4::kFencePolls, 1000, "the wait budget is a fixed number of polls");

    adreno::pm4::FenceWaiter waiter = {};
    adreno::pm4::fenceWaitBegin(&waiter, 9);
    expect(adreno::pm4::fenceWaitPoll(&waiter, 8) == adreno::pm4::FenceWait::Pending,
           "a fence one short of the value keeps waiting");
    expect(adreno::pm4::fenceWaitPoll(&waiter, 9) == adreno::pm4::FenceWait::Complete,
           "the value completes the wait");
    expectEq(waiter.polls, 0, "and resets the poll count");

    adreno::pm4::fenceWaitBegin(&waiter, 1);
    u32 pending = 0;
    for (u32 poll = 0; poll < adreno::pm4::kFencePolls - 1; poll++) {
        if (adreno::pm4::fenceWaitPoll(&waiter, 0) == adreno::pm4::FenceWait::Pending) { pending++; }
    }
    expectEq(pending, adreno::pm4::kFencePolls - 1, "one poll short of the budget is still pending");
    expect(adreno::pm4::fenceWaitPoll(&waiter, 0) == adreno::pm4::FenceWait::TimedOut,
           "the poll that spends the budget times out");
    expect(adreno::pm4::fenceWaitPoll(&waiter, 1) == adreno::pm4::FenceWait::TimedOut,
           "a late fence does not revive a timed-out submission");

    adreno::pm4::fenceWaitBegin(&waiter, 0xfffffff0u);
    expect(adreno::pm4::fenceWaitPoll(&waiter, 0x00000004u) == adreno::pm4::FenceWait::Complete,
           "a wrapped counter completes a fence near the top of the range");
    expect(adreno::pm4::fenceWaitPoll(nullptr, 0) == adreno::pm4::FenceWait::TimedOut,
           "no waiter is not a wait");

    // --- the ladder -------------------------------------------------------
    expect(adreno::kWrittenRung == adreno::Rung::Identity,
           "only rung 1 is written, and the driver says so");
    expect(adreno::kCoreRung == adreno::Rung::Ring,
           "cores exist through rung 4; 5's fence half and 6's addon audit are separate");
    // Both GDSCRs are read through the MMSS window the ADT gives. They sit at
    // 0x4024 and 0x4034, so a 0x1000-byte window would put every read past its
    // end; this is the test that noticed.
    expect(lumia950::kGdscrGxOffset + 4 <= lumia950::kMmssSize,
           "gdsc_oxili_gx is inside the MMSS window");
    expect(lumia950::kGdscrCxOffset + 4 <= lumia950::kMmssSize,
           "gdsc_oxili_cx is inside the MMSS window");
    expect(lumia950::kRegCpMeCntl << lumia950::kRegisterShift < lumia950::kGpuSize,
           "the last CP register is inside the KGSL window");
    expect(std::strcmp(adreno::rungName(adreno::Rung::Identity), "identity") == 0,
           "rung 1 is named identity");
    expect(std::strcmp(adreno::rungName(adreno::Rung::Metal), "metal") == 0,
           "rung 6 is named metal");
    expect(std::strcmp(adreno::rungName(adreno::Rung::Blit), "blit") == 0,
           "rung 5 is named blit");

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
