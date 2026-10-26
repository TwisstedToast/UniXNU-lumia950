#include "../PollingHost.h"
#include "../SdhciRegs.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace sdhci;

namespace {
unsigned checks = 0;
unsigned failures = 0;

void expect(bool condition, const char *name)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

enum class Fault { None, Command, Buffer, Word, Transfer };

struct Command {
    u8 index;
    u32 argument;
    u16 flags;
    u16 transfer;
};

struct Write {
    u32 offset;
    u32 value;
    u32 width;
};

struct FakeHost {
    std::array<u8, 512> extCsd = {};
    std::array<u8, 512> data = {};
    std::array<u32, 4> response = {};
    std::vector<Command> commands;
    std::vector<Write> writes;
    u32 status = 0;
    u32 statusEnable = 0;
    u32 signalEnable = 0xffffffff;
    u32 argument = 0;
    u32 state = 4;
    u32 responseErrors = 0;
    u32 errorBits = kErrDataCrc;
    u32 cardSectorCount = 0x04000000;
    u32 polls = 0;
    u32 commandAfter = 2;
    u32 bufferAfter = 4;
    u32 completionAfter = 3;
    u32 completionPolls = 0;
    u32 wordCount = 0;
    u32 faultWord = 7;
    u32 resetPolls = 0;
    u32 resetCount = 0;
    u32 delayCount = 0;
    u32 inhibit = 0;
    u32 presentDelay = 0;
    u32 busyPolls = 0;
    u32 readyCardAfter = 0;
    u16 rca = 1;
    u16 transfer = 0;
    u16 blockSize = 0;
    u16 blockCount = 0;
    u8 activeCommand = 0;
    u8 reset = 0;
    bool active = false;
    bool hasData = false;
    bool busyResponse = false;
    bool noResponse = false;
    bool commandEmitted = false;
    bool bufferEmitted = false;
    bool transferEmitted = false;
    bool errorEmitted = false;
    bool stuckReset = false;
    bool missingBuffer = false;
    bool missingTransfer = false;
    bool badCsd = false;
    bool wrongCsdState = false;
    bool unexpectedIndexCheck = false;
    bool badTransferMode = false;
    bool rejectedWrite = false;
    bool staleTransfer = false;
    Fault fault = Fault::None;
    u8 onlyFaultCommand = 0xff;

    FakeHost()
    {
        extCsd[192] = 7;
        extCsd[194] = 2;
        setSectors(cardSectorCount);
    }

    void setSectors(u32 sectors)
    {
        cardSectorCount = sectors;
        for (u32 byte = 0; byte < 4; ++byte) extCsd[212 + byte] = static_cast<u8>(sectors >> (8 * byte));
    }

    bool hasFault(Fault phase) const
    {
        return fault == phase && (onlyFaultCommand == 0xff || onlyFaultCommand == activeCommand);
    }

    void emit(u32 bits)
    {
        status |= bits & statusEnable;
    }

    void tick()
    {
        if (!active) return;
        ++polls;
        if (hasFault(Fault::Command) && !errorEmitted && polls >= commandAfter) {
            errorEmitted = true;
            emit(errorBits);
        }
        if (!noResponse && !commandEmitted && polls >= commandAfter) {
            commandEmitted = true;
            emit(kIntCommandComplete);
        }
        if (hasData && commandEmitted && !bufferEmitted && polls >= bufferAfter && !missingBuffer) {
            bufferEmitted = true;
            emit(kIntBufferReadReady);
            if (hasFault(Fault::Buffer)) emit(errorBits);
        }
        if (hasFault(Fault::Word) && wordCount == faultWord) emit(errorBits);
        if ((hasData && wordCount == 128) || (busyResponse && commandEmitted)) {
            ++completionPolls;
            if (hasFault(Fault::Transfer) && completionPolls >= completionAfter) emit(errorBits);
            if (!missingTransfer && !transferEmitted && completionPolls >= completionAfter) {
                transferEmitted = true;
                emit(kIntTransferComplete);
            }
        }
    }

    void launch(u16 value)
    {
        activeCommand = static_cast<u8>(value >> 8);
        const u16 flags = value & 0xff;
        commands.push_back({activeCommand, argument, flags, transfer});
        active = true;
        hasData = (flags & kCmdDataPresent) != 0;
        busyResponse = (flags & 3) == kCmdResponse48Busy;
        noResponse = false;
        commandEmitted = bufferEmitted = transferEmitted = errorEmitted = false;
        polls = wordCount = completionPolls = 0;
        response = {};
        if (activeCommand == kCmd13SendStatus) {
            noResponse = (argument >> 16) != rca;
            const bool ready = ++busyPolls >= readyCardAfter;
            response[0] = (state << 9) | (ready ? 0x100 : 0) | responseErrors;
        } else if (activeCommand == kCmd7SelectCard) {
            state = argument ? 4 : 3;
            busyPolls = 0;
            response[0] = 0x100 | (state << 9) | responseErrors;
        } else if (activeCommand == kCmd9SendCsd) {
            wrongCsdState |= state != 3;
            unexpectedIndexCheck |= (flags & kCmdIndexCheck) != 0;
            const u32 csd[4] = {badCsd ? 0x10000000u : 0xd0000000u,
                                 0x000903ff, 0xc0038000, 0};
            for (u32 word = 0; word < 4; ++word) {
                response[word] = csd[3 - word] >> 8;
                if (word != 3) response[word] |= csd[2 - word] << 24;
            }
        } else if (hasData) {
            badTransferMode |= transfer != (kModeBlockCountEnable | kModeDirectionRead) ||
                               blockSize != 512 || blockCount != 1;
            response[0] = 0x900 | responseErrors;
            for (u32 byte = 0; byte < 512; ++byte) data[byte] = static_cast<u8>(byte * 13 + argument * 7);
            if (argument == 1) std::memcpy(data.data(), "EFI PART", 8);
        }
        if (staleTransfer) status |= kIntTransferComplete;
    }

    static u8 read8(void *context, u32 offset)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        expect(offset == kRegSoftwareReset, "only software-reset byte is read by core");
        if (!fake.stuckReset && ++fake.resetPolls >= 2) fake.reset = 0;
        return fake.reset;
    }

    static u32 read32(void *context, u32 offset)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        if (offset == kRegInterruptStatus) {
            fake.tick();
            return fake.status;
        }
        if (offset == kRegPresentState) {
            if (fake.presentDelay) {
                --fake.presentDelay;
                return kPresentCmdInhibit | kPresentDataInhibit;
            }
            return fake.inhibit | (fake.bufferEmitted && fake.wordCount < 128 ? kPresentBufferRead : 0);
        }
        if (offset >= kRegResponse && offset < kRegResponse + 16) return fake.response[(offset - kRegResponse) / 4];
        if (offset == kRegBufferPort) {
            if (!fake.bufferEmitted || fake.wordCount >= 128) {
                expect(false, "buffer is read only while a block is ready");
                return 0;
            }
            const auto &bytes = fake.activeCommand == kCmd8SendExtCsd ? fake.extCsd : fake.data;
            u32 value = 0;
            for (u32 byte = 0; byte < 4; ++byte) value |= static_cast<u32>(bytes[fake.wordCount * 4 + byte]) << (byte * 8);
            ++fake.wordCount;
            return value;
        }
        expect(false, "no unknown MMIO read");
        return 0;
    }

    static void write8(void *context, u32 offset, u8 value)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        fake.writes.push_back({offset, value, 1});
        if (offset == kRegSoftwareReset) {
            fake.rejectedWrite |= value != (kResetCommand | kResetData);
            fake.reset = value;
            fake.resetPolls = 0;
            ++fake.resetCount;
            fake.active = false;
        } else if (offset == kRegTimeoutControl) {
            fake.rejectedWrite |= value != 0x0e;
        } else {
            fake.rejectedWrite = true;
        }
    }

    static void write16(void *context, u32 offset, u16 value)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        fake.writes.push_back({offset, value, 2});
        if (offset == kRegBlockSize) fake.blockSize = value;
        else if (offset == kRegBlockCount) fake.blockCount = value;
        else if (offset == kRegTransferMode) fake.transfer = value;
        else if (offset == kRegCommand) fake.launch(value);
        else fake.rejectedWrite = true;
    }

    static void write32(void *context, u32 offset, u32 value)
    {
        auto &fake = *static_cast<FakeHost *>(context);
        fake.writes.push_back({offset, value, 4});
        if (offset == kRegArgument) fake.argument = value;
        else if (offset == kRegInterruptStatus) fake.status &= ~value;
        else if (offset == kRegInterruptEnable) fake.statusEnable = value;
        else if (offset == kRegInterruptSignal) fake.signalEnable = value;
        else fake.rejectedWrite = true;
    }

    static void delay(void *context, u32)
    {
        ++static_cast<FakeHost *>(context)->delayCount;
    }

    RegisterIo io()
    {
        return {this, read8, read32, write8, write16, write32, delay};
    }
};

PollLimits shortLimits()
{
    return {10, 30, 10, 1, 4};
}

void configured(FakeHost &fake, PollingHost &host)
{
    expect(host.configure(fake.io(), shortLimits()), "valid MMIO callbacks configure");
}

void testStartup()
{
    FakeHost fake;
    PollingHost host;
    configured(fake, host);
    fake.status = kIntTransferComplete | kIntBufferReadReady | kErrDataTimeout | kIntCardInterrupt;
    expect(host.start(), "warm controller starts");
    expect(host.ready(), "host is ready after reset acknowledgement");
    expect(fake.signalEnable == 0, "firmware IRQ signaling is disabled for polling");
    expect((fake.statusEnable & (kIntCommandComplete | kIntTransferComplete | kIntBufferReadReady)) ==
           (kIntCommandComplete | kIntTransferComplete | kIntBufferReadReady), "PIO status latching is enabled");
    expect(fake.status == kIntCardInterrupt, "only owned status is acknowledged");
    expect(!fake.rejectedWrite, "clock, power, bus width and vendor windows remain untouched");
    expect(!host.configure(fake.io()), "callbacks cannot be replaced while running");
    host.stop();
    expect(!host.ready(), "stop prevents new commands");

    FakeHost stuck;
    stuck.stuckReset = true;
    PollingHost failed;
    configured(stuck, failed);
    expect(!failed.start(), "stuck CMD/DAT reset fails start");
    expect(failed.result().error == HostError::ResetTimeout, "reset failure is diagnostic");
    expect(stuck.delayCount == shortLimits().resetPolls, "reset wait is bounded");
    u32 response[4];
    expect(!failed.command(13, 1u << 16, ResponseType::R1, response), "no command after failed reset");
    expect(stuck.commands.empty(), "failed reset never reaches command register");

    RegisterIo invalid = fake.io();
    invalid.read32 = nullptr;
    PollingHost missing;
    expect(!missing.configure(invalid), "missing callback is refused");
    expect(!missing.start(), "invalid IO never touches hardware");
    PollLimits invalidLimits = shortLimits();
    invalidLimits.transferPolls = 0;
    expect(!missing.configure(fake.io(), invalidLimits), "zero transfer budget is refused");
    invalidLimits = shortLimits();
    invalidLimits.commandPolls = 0xffffffff;
    expect(!missing.configure(fake.io(), invalidLimits), "overflow-prone poll budget is refused");
}

void testCommands()
{
    FakeHost fake;
    PollingHost host;
    configured(fake, host);
    expect(host.start(), "command host starts");
    fake.inhibit = kPresentDataInhibit;
    u32 response[4] = {};
    expect(host.command(13, 1u << 16, ResponseType::R1, response), "CMD13 can inspect a DAT-inhibited card");
    expect(response[0] == 0x900, "short R1 response is not R2-shifted");
    expect(fake.commands.back().flags == (kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck),
           "R1 has response CRC and index checks");
    fake.inhibit = 0;
    expect(host.command(7, 0, ResponseType::None, nullptr), "deselect does not expect an R1 timeout");
    expect(fake.commands.back().flags == 0, "CMD7 deselect carries no-response flags");
    expect(host.command(9, 1u << 16, ResponseType::R2, response), "standby card supplies CSD");
    expect(response[0] == 0xd0000000 && response[1] == 0x000903ff && response[2] == 0xc0038000,
           "controller R2 words become canonical CSD bit positions");
    expect(!fake.wrongCsdState && !fake.unexpectedIndexCheck, "CSD uses standby state without opcode check");
    expect(host.command(7, 1u << 16, ResponseType::R1Busy, response), "R1b waits for busy-end completion");
    expect(fake.transferEmitted, "R1b command completion alone is insufficient");
    expect(fake.commands.back().transfer == 0, "nondata command clears inherited DMA mode");
    fake.inhibit = kPresentCmdInhibit;
    const auto commandsBefore = fake.commands.size();
    expect(!host.command(13, 1u << 16, ResponseType::R1, response), "inhibited command times out");
    expect(host.result().error == HostError::InhibitTimeout, "inhibit has its own failure code");
    expect(fake.commands.size() == commandsBefore, "inhibited command is never issued");

    FakeHost bad;
    bad.responseErrors = 1u << 22;
    PollingHost badHost;
    configured(bad, badHost);
    expect(badHost.start(), "R1 error host starts");
    expect(!badHost.command(13, 1u << 16, ResponseType::R1, response), "illegal-command R1 is not success");
    expect(badHost.result().error == HostError::CardStatus, "R1 error is distinguished from controller error");
    expect((badHost.result().cardStatus & (1u << 22)) != 0, "full R1 status is preserved");

    FakeHost timed;
    timed.rca = 2;
    PollingHost timeout;
    configured(timed, timeout);
    expect(timeout.start(), "timeout host starts");
    expect(!timeout.command(13, 1u << 16, ResponseType::R1, response), "nonexistent RCA times out");
    expect(timeout.result().error == HostError::CommandTimeout, "missing response is a command timeout");
    expect(timed.polls == shortLimits().commandPolls, "command wait is bounded");
}

void testReads()
{
    for (u32 delay = 1; delay <= 12; ++delay) {
        FakeHost fake;
        fake.completionAfter = delay;
        fake.commandAfter = 1 + delay % 3;
        fake.bufferAfter = fake.commandAfter + 1 + delay % 4;
        PollingHost host;
        configured(fake, host);
        expect(host.start(), "read host starts");
        std::array<u8, 514> destination;
        destination.fill(0xa5);
        expect(host.read512(17, 123, destination.data() + 1), "delayed transfer completion succeeds");
        expect(!std::memcmp(destination.data() + 1, fake.data.data(), 512), "unaligned output matches all 512 FIFO bytes");
        expect(destination.front() == 0xa5 && destination.back() == 0xa5, "read preserves destination guards");
        expect(fake.wordCount == 128, "PIO drains exactly one complete block");
        expect(host.result().bytesTransferred == 512, "verified byte count is published only on success");
        expect((fake.status & (kIntCommandComplete | kIntBufferReadReady | kIntTransferComplete)) == 0,
               "owned completion events are acknowledged");
        expect(!fake.badTransferMode && !fake.rejectedWrite, "read never enables DMA or writes a data sector");
    }

    for (Fault phase : {Fault::Command, Fault::Buffer, Fault::Word, Fault::Transfer}) {
        FakeHost fake;
        fake.fault = phase;
        PollingHost host;
        configured(fake, host);
        expect(host.start(), "fault-injection host starts");
        std::array<u8, 512> destination;
        destination.fill(0xa5);
        expect(!host.read512(17, 1, destination.data()), "controller fault fails the read");
        expect(host.result().error == HostError::Controller, "high error bit fails without summary ERROR bit");
        expect(host.result().interruptStatus & kErrDataCrc, "original controller error is retained");
        expect(host.result().bytesTransferred == 0, "failed read publishes zero bytes");
        expect(!host.ready(), "failed transfer prevents implicit reuse without reattachment");
        bool unchanged = true;
        for (u8 value : destination) unchanged &= value == 0xa5;
        expect(unchanged, "failed transfer never leaks partial FIFO data");
        if (phase == Fault::Word) expect(fake.wordCount == fake.faultWord, "mid-FIFO error stops immediately");
    }

    for (bool missingBuffer : {false, true}) {
        FakeHost fake;
        fake.missingBuffer = missingBuffer;
        fake.missingTransfer = !missingBuffer;
        PollingHost host;
        configured(fake, host);
        expect(host.start(), "stalled host starts");
        std::array<u8, 512> destination = {};
        expect(!host.read512(17, 1, destination.data()), "stalled data phase fails");
        expect(host.result().error == HostError::DataTimeout, "data timeout is diagnosed");
        expect(fake.delayCount <= shortLimits().transferPolls + shortLimits().commandPolls + 2,
               "whole-block polling budget is shared across buffer and completion");
        if (missingBuffer) expect(fake.wordCount == 0, "missing buffer event does not touch FIFO");
    }

    FakeHost invalid;
    PollingHost invalidHost;
    configured(invalid, invalidHost);
    expect(invalidHost.start(), "invalid-read host starts");
    std::array<u8, 512> destination = {};
    expect(!invalidHost.read512(24, 1, destination.data()), "PIO read interface refuses write command");
    expect(invalid.commands.empty(), "invalid read never reaches hardware command register");
}

void testWarmCard()
{
    const u16 candidates[] = {1, 2, 0};
    FakeHost fake;
    fake.rca = 2;
    fake.transfer = 0x3f;
    fake.status = kIntTransferComplete;
    PollingHost host;
    configured(fake, host);
    WarmCard card;
    expect(card.attach(host, candidates, 3), "firmware-selected card attaches after skipping absent RCA");
    expect(card.ready() && card.info().relativeAddress == 2, "successful RCA is retained");
    expect(card.info().capacityBytes == (32ULL << 30), "capacity comes from EXT_CSD SEC_COUNT");
    expect(card.info().csd.structure == 3, "modern CSD is accepted through actual R2 MMIO path");
    expect(fake.resetCount == 2, "wrong-RCA command timeout is recovered before next candidate");
    expect(!fake.wrongCsdState && !fake.unexpectedIndexCheck, "warm CSD is read while deselected with correct R2 flags");
    expect(fake.state == 4 && card.info().extCsd.partitionAccess == 0, "card returns to selected user partition");
    expect(!fake.rejectedWrite && !fake.badTransferMode, "warm handoff preserves firmware clock and bus configuration");
    bool initializationCommand = false;
    bool extCsdRead = false;
    for (const Command &command : fake.commands) {
        initializationCommand |= command.index <= 3 || command.index == 6 ||
                                 command.index == 16 || command.index >= 24;
        extCsdRead |= command.index == 8;
    }
    expect(!initializationCommand && extCsdRead, "no reinitialization, partition switch or write is sent");
    std::array<u8, 512> gpt = {};
    expect(card.readBlock(1, gpt.data()), "attached card reads GPT sector");
    expect(!std::memcmp(gpt.data(), "EFI PART", 8), "GPT bytes traverse the real transport core");
    expect(card.readBlock(card.info().capacityBytes / 512 - 1, gpt.data()), "last sector is addressable");
    const auto commandsBefore = fake.commands.size();
    expect(!card.readBlock(card.info().capacityBytes / 512, gpt.data()), "one-past-capacity read is refused");
    expect(!card.readBlock(~0ULL, gpt.data()), "64-bit LBA cannot truncate into CMD17 argument");
    expect(fake.commands.size() == commandsBefore, "out-of-range access emits no command");
    fake.onlyFaultCommand = 17;
    fake.fault = Fault::Word;
    expect(!card.readBlock(1, gpt.data()), "attached medium fails a damaged transfer");
    expect(!card.ready(), "damaged transfer detaches card instead of serving stale data");
    card.detach();
    expect(!card.readBlock(1, gpt.data()), "detached card emits no read");

    for (u32 initialState : {3u, 4u}) {
        FakeHost standby;
        standby.state = initialState;
        PollingHost standbyHost;
        configured(standby, standbyHost);
        WarmCard standbyCard;
        expect(standbyCard.attach(standbyHost, candidates, 1), "standby and transfer handoffs both attach");
    }

    for (u32 mutation = 0; mutation < 10; ++mutation) {
        FakeHost broken;
        WarmCardError expected = WarmCardError::UnsupportedCard;
        switch (mutation) {
        case 0: broken.extCsd[179] = 1; expected = WarmCardError::WrongPartition; break;
        case 1: broken.extCsd[179] = 3; expected = WarmCardError::WrongPartition; break;
        case 2: broken.extCsd[61] = 1; break;
        case 3: broken.extCsd[192] = 1; break;
        case 4: broken.setSectors(0); break;
        case 5: broken.extCsd[15] = 1; expected = WarmCardError::CommandQueue; break;
        case 6: broken.extCsd[194] = 3; expected = WarmCardError::InvalidExtCsd; break;
        case 7: broken.badCsd = true; expected = WarmCardError::InvalidCsd; break;
        case 8: broken.state = 5; expected = WarmCardError::UnexpectedState; break;
        case 9: broken.setSectors(0x00400000); break;
        }
        PollingHost brokenHost;
        configured(broken, brokenHost);
        WarmCard brokenCard;
        expect(!brokenCard.attach(brokenHost, candidates, 1), "unsupported card handoff is rejected");
        expect(brokenCard.error() == expected, "specific attach failure is retained");
        expect(!brokenCard.ready(), "unsupported medium is not published");
        std::array<u8, 512> bytes = {};
        const auto before = broken.commands.size();
        expect(!brokenCard.readBlock(1, bytes.data()), "unsupported medium cannot read another partition");
        expect(broken.commands.size() == before, "unsupported medium emits no follow-up I/O");
    }
}
}

int main()
{
    testStartup();
    testCommands();
    testReads();
    testWarmCard();
    std::printf("PollingHost: %u checks, %u failures (simulated MMIO, not hardware)\n", checks, failures);
    return failures ? 1 : 0;
}
