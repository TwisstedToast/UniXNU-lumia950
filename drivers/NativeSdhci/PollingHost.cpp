#include "PollingHost.h"
#include "SdhciRegs.h"

namespace sdhci {
namespace {
constexpr u32 kErrorStatus = 0xffff8000u;
constexpr u32 kPollingStatus = kIntCommandComplete | kIntTransferComplete |
                               kIntBufferReadReady | kIntError | 0x03ff0000u;
constexpr u32 kCardErrors = 0xffff0000u | (1u << 7);
constexpr u32 kReadyForData = 1u << 8;
constexpr u32 kStateStandby = 3;
constexpr u32 kStateTransfer = 4;

u32 cardState(u32 response)
{
    return (response >> 9) & 15;
}
}

bool PollingHost::validIo() const
{
    return io.read8 && io.read32 && io.write8 && io.write16 && io.write32 && io.delayUs;
}

bool PollingHost::configure(const RegisterIo &registers, const PollLimits &pollLimits)
{
    if (running) return false;
    io = registers;
    limits = pollLimits;
    last = {};
    if (!validIo() || !limits.commandPolls || limits.commandPolls > 1000000 ||
        !limits.transferPolls || limits.transferPolls > 1000000 ||
        !limits.resetPolls || limits.resetPolls > 1000000 ||
        !limits.delayUs || limits.delayUs > 1000 ||
        !limits.cardPolls || limits.cardPolls > 1000) {
        io = {};
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }
    return true;
}

bool PollingHost::fail(HostError error, HostPhase phase, u32 status)
{
    last.error = error;
    last.phase = phase;
    last.interruptStatus = status;
    running = false;
    return false;
}

void PollingHost::acknowledge(u32 status)
{
    io.write32(io.context, kRegInterruptStatus, status & (kPollingStatus | kErrorStatus));
}

bool PollingHost::start()
{
    running = false;
    last = {};
    if (!validIo()) return fail(HostError::InvalidArgument, HostPhase::Reset);
    io.write32(io.context, kRegInterruptSignal, 0);
    io.write8(io.context, kRegSoftwareReset, kResetCommand | kResetData);
    bool reset = false;
    for (u32 poll = 0; poll < limits.resetPolls; ++poll) {
        if (!(io.read8(io.context, kRegSoftwareReset) & (kResetCommand | kResetData))) {
            reset = true;
            break;
        }
        io.delayUs(io.context, limits.delayUs);
    }
    if (!reset) return fail(HostError::ResetTimeout, HostPhase::Reset);
    io.write8(io.context, kRegTimeoutControl, 0x0e);
    io.write32(io.context, kRegInterruptEnable, kPollingStatus);
    acknowledge(kPollingStatus);
    running = true;
    return true;
}

void PollingHost::stop()
{
    if (validIo()) io.write32(io.context, kRegInterruptSignal, 0);
    running = false;
}

bool PollingHost::waitStatus(u32 wanted, u32 &remaining, HostPhase phase, HostError timeout)
{
    u32 status = 0;
    while (remaining) {
        --remaining;
        status = io.read32(io.context, kRegInterruptStatus);
        if (status & kErrorStatus) return fail(HostError::Controller, phase, status);
        if ((status & wanted) == wanted) return true;
        io.delayUs(io.context, limits.delayUs);
    }
    return fail(timeout, phase, status);
}

bool PollingHost::issue(u8 index, u32 argument, ResponseType type, bool data, u32 response[4])
{
    last = {};
    last.command = index;
    if (!running) return fail(HostError::NotReady, HostPhase::Idle);
    if (index > 63 || static_cast<u8>(type) > static_cast<u8>(ResponseType::R2) ||
        (data && type != ResponseType::R1) || (type != ResponseType::None && !response)) {
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }

    const u32 inhibit = kPresentCmdInhibit |
                        ((data || type == ResponseType::R1Busy) ? static_cast<u32>(kPresentDataInhibit) : 0u);
    bool available = false;
    for (u32 poll = 0; poll < limits.commandPolls; ++poll) {
        if (!(io.read32(io.context, kRegPresentState) & inhibit)) {
            available = true;
            break;
        }
        io.delayUs(io.context, limits.delayUs);
    }
    if (!available) return fail(HostError::InhibitTimeout, HostPhase::Inhibit);

    acknowledge(kPollingStatus | kErrorStatus);
    if (data) {
        io.write16(io.context, kRegBlockSize, 512);
        io.write16(io.context, kRegBlockCount, 1);
    }
    io.write16(io.context, kRegTransferMode,
               data ? kModeBlockCountEnable | kModeDirectionRead : 0);
    u16 flags = data ? kCmdDataPresent : 0;
    switch (type) {
    case ResponseType::None: break;
    case ResponseType::R1: flags |= kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck; break;
    case ResponseType::R1Busy: flags |= kCmdResponse48Busy | kCmdCrcCheck | kCmdIndexCheck; break;
    case ResponseType::R2: flags |= kCmdResponse136 | kCmdCrcCheck; break;
    }
    io.write32(io.context, kRegArgument, argument);
    io.write16(io.context, kRegCommand, static_cast<u16>((index << 8) | flags));
    u32 remaining = limits.commandPolls;
    if (!waitStatus(kIntCommandComplete, remaining, HostPhase::Command, HostError::CommandTimeout)) {
        return false;
    }
    if (response) {
        for (u32 word = 0; word < 4; ++word) response[word] = 0;
        if (type == ResponseType::R2) {
            u32 registers[4];
            for (u32 word = 0; word < 4; ++word) {
                registers[word] = io.read32(io.context, kRegResponse + word * 4);
            }
            normalizeResponse136(registers, response);
        } else if (type != ResponseType::None) {
            response[0] = io.read32(io.context, kRegResponse);
        }
    }
    acknowledge(kIntCommandComplete);
    if (type == ResponseType::R1 || type == ResponseType::R1Busy) {
        last.cardStatus = response[0];
        if (response[0] & kCardErrors) return fail(HostError::CardStatus, HostPhase::Card);
    }
    if (type == ResponseType::R1Busy) {
        remaining = limits.transferPolls;
        if (!waitStatus(kIntTransferComplete, remaining, HostPhase::Transfer, HostError::DataTimeout)) {
            return false;
        }
        acknowledge(kIntTransferComplete);
    }
    return true;
}

bool PollingHost::command(u8 index, u32 argument, ResponseType type, u32 response[4])
{
    return issue(index, argument, type, false, response);
}

bool PollingHost::read512(u8 index, u32 argument, u8 *destination)
{
    if (!destination || (index != kCmd8SendExtCsd && index != kCmd17ReadSingleBlock)) {
        last = {};
        return fail(HostError::InvalidArgument, HostPhase::Idle);
    }
    u32 response[4];
    if (!issue(index, argument, ResponseType::R1, true, response)) return false;
    u32 remaining = limits.transferPolls;
    if (!waitStatus(kIntBufferReadReady, remaining, HostPhase::Buffer, HostError::DataTimeout)) {
        return false;
    }
    bool readable = false;
    while (remaining) {
        --remaining;
        const u32 status = io.read32(io.context, kRegInterruptStatus);
        if (status & kErrorStatus) return fail(HostError::Controller, HostPhase::Buffer, status);
        if (io.read32(io.context, kRegPresentState) & kPresentBufferRead) {
            readable = true;
            break;
        }
        io.delayUs(io.context, limits.delayUs);
    }
    if (!readable) return fail(HostError::DataTimeout, HostPhase::Buffer);
    u8 staging[512];
    for (u32 word = 0; word < 128; ++word) {
        const u32 status = io.read32(io.context, kRegInterruptStatus);
        if (status & kErrorStatus) return fail(HostError::Controller, HostPhase::Buffer, status);
        const u32 value = io.read32(io.context, kRegBufferPort);
        for (u32 byte = 0; byte < 4; ++byte) {
            staging[word * 4 + byte] = static_cast<u8>(value >> (byte * 8));
        }
    }
    acknowledge(kIntBufferReadReady);
    if (!waitStatus(kIntTransferComplete, remaining, HostPhase::Transfer, HostError::DataTimeout)) {
        return false;
    }
    acknowledge(kIntTransferComplete);
    for (u32 byte = 0; byte < 512; ++byte) destination[byte] = staging[byte];
    last.bytesTransferred = 512;
    return true;
}

bool PollingHost::waitCardReady(u16 relativeAddress, u32 state)
{
    if (!relativeAddress || (state != kStateStandby && state != kStateTransfer)) {
        last = {};
        return fail(HostError::InvalidArgument, HostPhase::Card);
    }
    for (u32 poll = 0; poll < limits.cardPolls; ++poll) {
        u32 response[4];
        if (!command(kCmd13SendStatus, static_cast<u32>(relativeAddress) << 16,
                     ResponseType::R1, response)) return false;
        if ((response[0] & kReadyForData) && cardState(response[0]) == state) return true;
        io.delayUs(io.context, 1000);
    }
    return fail(HostError::CardState, HostPhase::Card);
}

bool WarmCard::fail(WarmCardError error)
{
    lastError = error;
    attached = false;
    return false;
}

void WarmCard::detach()
{
    attached = false;
    host = nullptr;
}

bool WarmCard::attach(PollingHost &controller, const u16 *candidates, u32 count)
{
    detach();
    card = {};
    lastError = WarmCardError::None;
    host = &controller;
    if (!candidates || !count || count > 16) return fail(WarmCardError::InvalidArgument);
    u32 response[4];
    bool selected = false;
    for (u32 candidate = 0; candidate < count; ++candidate) {
        if (!candidates[candidate]) continue;
        if (!host->start()) return fail(WarmCardError::Host);
        if (!host->command(kCmd13SendStatus, static_cast<u32>(candidates[candidate]) << 16,
                           ResponseType::R1, response)) continue;
        const u32 state = cardState(response[0]);
        if (!(response[0] & kReadyForData) || (state != kStateStandby && state != kStateTransfer)) {
            return fail(WarmCardError::UnexpectedState);
        }
        card.relativeAddress = candidates[candidate];
        selected = state == kStateTransfer;
        break;
    }
    if (!card.relativeAddress) return fail(WarmCardError::NoRelativeAddress);
    if (selected && !host->command(kCmd7SelectCard, 0, ResponseType::None, nullptr)) {
        return fail(WarmCardError::Host);
    }
    if (!host->waitCardReady(card.relativeAddress, kStateStandby) ||
        !host->command(kCmd9SendCsd, static_cast<u32>(card.relativeAddress) << 16,
                       ResponseType::R2, response)) return fail(WarmCardError::Host);
    const bool validCsd = decodeCsd(response, &card.csd);
    if (!host->command(kCmd7SelectCard, static_cast<u32>(card.relativeAddress) << 16,
                       ResponseType::R1, response) ||
        !host->waitCardReady(card.relativeAddress, kStateTransfer)) return fail(WarmCardError::Host);
    if (!validCsd) return fail(WarmCardError::InvalidCsd);
    if (card.csd.mmcVersion < 4) return fail(WarmCardError::UnsupportedCard);
    u8 extCsd[512];
    if (!host->read512(kCmd8SendExtCsd, 0, extCsd)) return fail(WarmCardError::Host);
    if (!decodeExtCsd(extCsd, sizeof extCsd, &card.extCsd)) return fail(WarmCardError::InvalidExtCsd);
    if (card.extCsd.partitionAccess) return fail(WarmCardError::WrongPartition);
    if (card.extCsd.commandQueueEnabled) return fail(WarmCardError::CommandQueue);
    if (card.extCsd.revision < 2 || card.extCsd.sectorBytes != 512 ||
        card.extCsd.capacityBytes <= (2ULL << 30)) return fail(WarmCardError::UnsupportedCard);
    if (!host->waitCardReady(card.relativeAddress, kStateTransfer)) return fail(WarmCardError::Host);
    card.capacityBytes = card.extCsd.capacityBytes;
    attached = true;
    return true;
}

bool WarmCard::readBlock(u64 lba, u8 *destination)
{
    if (!ready() || !destination || lba >= card.capacityBytes / 512 || lba > 0xffffffffULL) {
        return false;
    }
    if (!host->read512(kCmd17ReadSingleBlock, static_cast<u32>(lba), destination)) {
        return fail(WarmCardError::Host);
    }
    return true;
}

const char *hostErrorName(HostError error)
{
    switch (error) {
    case HostError::None: return "none";
    case HostError::InvalidArgument: return "invalid argument";
    case HostError::NotReady: return "not ready";
    case HostError::ResetTimeout: return "reset timeout";
    case HostError::InhibitTimeout: return "inhibit timeout";
    case HostError::CommandTimeout: return "command timeout";
    case HostError::DataTimeout: return "data timeout";
    case HostError::Controller: return "controller error";
    case HostError::CardStatus: return "card status error";
    case HostError::CardState: return "unexpected card state";
    }
    return "unknown";
}

}
