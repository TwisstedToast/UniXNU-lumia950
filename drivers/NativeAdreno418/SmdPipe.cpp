#include "SmdPipe.h"

namespace adreno {
namespace rpm {
namespace {

constexpr u32 kHalfBytes = 44;
constexpr u32 kStreamState = 0;
constexpr u32 kDtr = 4;
constexpr u32 kCts = 8;
constexpr u32 kCarrierDetect = 12;
constexpr u32 kDataWritten = 20;
constexpr u32 kDataRead = 24;
constexpr u32 kStateUpdated = 28;
constexpr u32 kMaskReceive = 32;
constexpr u32 kReadIndex = 36;
constexpr u32 kWriteIndex = 40;
constexpr u32 kPacketHeaderBytes = 20;

bool validIo(const SmdIo &io, u32 controlBytes, u32 fifoBytes, bool uncachedMappings)
{
    return uncachedMappings && controlBytes >= 2 * kHalfBytes && fifoBytes >= 512 &&
        fifoBytes <= 65536 && (fifoBytes & (fifoBytes - 1)) == 0 &&
        io.read32 && io.write32 && io.barrier && io.notify && io.delayUs;
}

bool read(SmdPipe &pipe, SmdSpace space, u32 offset, u32 *value)
{
    return pipe.io.read32(pipe.io.context, space, offset, value);
}

bool write(SmdPipe &pipe, SmdSpace space, u32 offset, u32 value)
{
    return pipe.io.write32(pipe.io.context, space, offset, value);
}

Transfer poison(SmdPipe &pipe, Transfer result)
{
    pipe.state = PipeState::Poisoned;
    return result;
}

bool ready(SmdPipe &pipe)
{
    u32 local = 0, remote = 0, localDtr = 0, remoteDtr = 0;
    return pipe.io.barrier(pipe.io.context) &&
        read(pipe, SmdSpace::Control, kStreamState, &local) &&
        read(pipe, SmdSpace::Control, kHalfBytes + kStreamState, &remote) &&
        read(pipe, SmdSpace::Control, kDtr, &localDtr) &&
        read(pipe, SmdSpace::Control, kHalfBytes + kDtr, &remoteDtr) &&
        local == 2 && (remote == 2 || remote == 3) && localDtr == 1 && remoteDtr == 1;
}

bool indices(SmdPipe &pipe, u32 half, u32 *reader, u32 *writer)
{
    return read(pipe, SmdSpace::Control, half + kReadIndex, reader) &&
        read(pipe, SmdSpace::Control, half + kWriteIndex, writer) &&
        *reader < pipe.fifoBytes && *writer < pipe.fifoBytes &&
        ((*reader | *writer) & 3) == 0;
}

u32 readWord(const u8 *bytes)
{
    return bytes[0] | (static_cast<u32>(bytes[1]) << 8) |
        (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
}

void writeWord(u8 *bytes, u32 value)
{
    for (u32 index = 0; index < 4; ++index) { bytes[index] = static_cast<u8>(value >> (index * 8)); }
}

Transfer transportSend(void *context, const u8 *bytes, u32 size)
{
    return sendPacket(*static_cast<SmdPipe *>(context), bytes, size);
}

Transfer transportReceive(void *context, u8 *bytes, u32 capacity, u32 *size)
{
    return receivePacket(*static_cast<SmdPipe *>(context), bytes, capacity, size);
}

void transportDelay(void *context, u32 microseconds)
{
    SmdPipe &pipe = *static_cast<SmdPipe *>(context);
    pipe.io.delayUs(pipe.io.context, microseconds);
}

}

Result adoptPipe(SmdPipe &pipe, const SmdIo &io, u32 controlBytes, u32 fifoBytes,
                 bool exclusiveOwnership, bool uncachedMappings)
{
    if (pipe.state != PipeState::Empty) { return Result::NotReady; }
    if (!exclusiveOwnership) { return Result::Unowned; }
    if (!validIo(io, controlBytes, fifoBytes, uncachedMappings)) { return Result::Invalid; }
    SmdPipe candidate = {io, PipeState::Ready, fifoBytes};
    u32 reader = 0, writer = 0;
    if (!ready(candidate) || !indices(candidate, 0, &reader, &writer) || reader != writer ||
        !indices(candidate, kHalfBytes, &reader, &writer) || reader != writer) { return Result::NotReady; }
    pipe = candidate;
    return Result::Ok;
}

Result openPipe(SmdPipe &pipe, const SmdIo &io, u32 controlBytes, u32 fifoBytes,
                bool exclusiveOwnership, bool uncachedMappings, u32 pollBudget, u32 delayUs)
{
    if (pipe.state != PipeState::Empty) { return Result::NotReady; }
    if (!exclusiveOwnership) { return Result::Unowned; }
    if (!validIo(io, controlBytes, fifoBytes, uncachedMappings) || !pollBudget ||
        pollBudget > 100000 || delayUs > 1000 || static_cast<u64>(pollBudget) * delayUs > 5000000) {
        return Result::Invalid;
    }
    SmdPipe candidate = {io, PipeState::Ready, fifoBytes};
    u32 local = 0, remote = 0, localDtr = 0, remoteDtr = 0, reader = 0, writer = 0;
    if (!io.barrier(io.context) ||
        !read(candidate, SmdSpace::Control, kStreamState, &local) || local != 0 ||
        !read(candidate, SmdSpace::Control, kHalfBytes + kStreamState, &remote) || (remote != 0 && remote != 4) ||
        !read(candidate, SmdSpace::Control, kDtr, &localDtr) || localDtr != 0 ||
        !read(candidate, SmdSpace::Control, kHalfBytes + kDtr, &remoteDtr) || remoteDtr > 1 ||
        (remote == 0 && remoteDtr != 0) ||
        !indices(candidate, 0, &reader, &writer) || reader != writer ||
        !indices(candidate, kHalfBytes, &reader, &writer) || reader != writer) {
        return Result::NotReady;
    }
    pipe = candidate;
    pipe.state = PipeState::Opening;
    if (!write(pipe, SmdSpace::Control, kHalfBytes + kReadIndex, 0) ||
        !write(pipe, SmdSpace::Control, kWriteIndex, 0) ||
        !write(pipe, SmdSpace::Control, kCts, 0) ||
        !write(pipe, SmdSpace::Control, kCarrierDetect, 0) ||
        !write(pipe, SmdSpace::Control, kMaskReceive, 0) || !io.barrier(io.context) ||
        !write(pipe, SmdSpace::Control, kStreamState, 1) ||
        !write(pipe, SmdSpace::Control, kStateUpdated, 1) ||
        !io.barrier(io.context) || !io.notify(io.context)) {
        poison(pipe, Transfer::IoError);
        return Result::IoError;
    }
    bool localOpened = false;
    for (u32 attempt = 0; attempt < pollBudget; ++attempt) {
        if (!io.barrier(io.context) || !read(pipe, SmdSpace::Control, kHalfBytes + kStreamState, &remote)) {
            poison(pipe, Transfer::IoError);
            return Result::IoError;
        }
        if (remote == 3 || remote > 4 || (localOpened && (remote == 0 || remote == 4))) {
            poison(pipe, Transfer::Closed);
            return Result::IoError;
        }
        if (!localOpened && (remote == 1 || remote == 2)) {
            if (!write(pipe, SmdSpace::Control, kHalfBytes + kStateUpdated, 0) ||
                !write(pipe, SmdSpace::Control, kDtr, 1) ||
                !write(pipe, SmdSpace::Control, kCts, 1) ||
                !write(pipe, SmdSpace::Control, kCarrierDetect, 1) || !io.barrier(io.context) ||
                !write(pipe, SmdSpace::Control, kStreamState, 2) ||
                !write(pipe, SmdSpace::Control, kStateUpdated, 1) ||
                !io.barrier(io.context) || !io.notify(io.context) ||
                !io.barrier(io.context) ||
                !read(pipe, SmdSpace::Control, kHalfBytes + kStreamState, &remote)) {
                poison(pipe, Transfer::IoError);
                return Result::IoError;
            }
            localOpened = true;
        }
        if (remote == 3 || remote > 4 || (localOpened && (remote == 0 || remote == 4))) {
            poison(pipe, Transfer::Closed);
            return Result::IoError;
        }
        if (localOpened && remote == 2) {
            if (!read(pipe, SmdSpace::Control, kHalfBytes + kDtr, &remoteDtr) || remoteDtr > 1) {
                poison(pipe, Transfer::IoError);
                return Result::IoError;
            }
            if (remoteDtr == 1) {
                if (!ready(pipe) || !indices(pipe, 0, &reader, &writer) || reader != 0 || writer != 0 ||
                    !indices(pipe, kHalfBytes, &reader, &writer) || reader != 0 || writer != 0 ||
                    !write(pipe, SmdSpace::Control, kHalfBytes + kStateUpdated, 0) || !io.barrier(io.context)) {
                    poison(pipe, Transfer::IoError);
                    return Result::IoError;
                }
                pipe.state = PipeState::Ready;
                return Result::Ok;
            }
        }
        if (attempt + 1 < pollBudget) { io.delayUs(io.context, delayUs); }
    }
    poison(pipe, Transfer::Closed);
    return Result::TimedOut;
}

Result closePipe(SmdPipe &pipe, u32 pollBudget, u32 delayUs)
{
    if (pipe.state != PipeState::Ready) { return Result::NotReady; }
    if (!pollBudget || pollBudget > 100000 || delayUs > 1000 ||
        static_cast<u64>(pollBudget) * delayUs > 5000000) { return Result::Invalid; }
    u32 reader = 0, writer = 0;
    if (!ready(pipe) || !indices(pipe, 0, &reader, &writer) || reader != writer ||
        !indices(pipe, kHalfBytes, &reader, &writer) || reader != writer) { return Result::NotReady; }
    pipe.state = PipeState::Closing;
    const SmdIo &io = pipe.io;
    if (!write(pipe, SmdSpace::Control, kDtr, 0) ||
        !write(pipe, SmdSpace::Control, kCts, 0) ||
        !write(pipe, SmdSpace::Control, kCarrierDetect, 0) ||
        !write(pipe, SmdSpace::Control, kMaskReceive, 1) || !io.barrier(io.context) ||
        !write(pipe, SmdSpace::Control, kStreamState, 4) ||
        !write(pipe, SmdSpace::Control, kStateUpdated, 1) ||
        !io.barrier(io.context) || !io.notify(io.context)) {
        poison(pipe, Transfer::IoError);
        return Result::IoError;
    }
    bool localClosed = false;
    for (u32 attempt = 0; attempt < pollBudget; ++attempt) {
        u32 remote = 0, remoteDtr = 0;
        if (!io.barrier(io.context) ||
            !read(pipe, SmdSpace::Control, kHalfBytes + kStreamState, &remote) ||
            !read(pipe, SmdSpace::Control, kHalfBytes + kDtr, &remoteDtr)) {
            poison(pipe, Transfer::IoError);
            return Result::IoError;
        }
        if (remote > 4) { poison(pipe, Transfer::Closed); return Result::IoError; }
        if (!localClosed && (remote == 4 || remote == 0)) {
            if (!write(pipe, SmdSpace::Control, kHalfBytes + kStateUpdated, 0) ||
                !write(pipe, SmdSpace::Control, kStreamState, 0) ||
                !write(pipe, SmdSpace::Control, kStateUpdated, 1) ||
                !io.barrier(io.context) || !io.notify(io.context)) {
                poison(pipe, Transfer::IoError);
                return Result::IoError;
            }
            localClosed = true;
        } else if (localClosed && ((remote == 0 && remoteDtr == 0) || (remote == 4 && remoteDtr <= 1))) {
            if (!write(pipe, SmdSpace::Control, kHalfBytes + kStateUpdated, 0) ||
                !io.barrier(io.context) || !indices(pipe, 0, &reader, &writer) || reader != writer ||
                !indices(pipe, kHalfBytes, &reader, &writer) || reader != writer) {
                poison(pipe, Transfer::IoError);
                return Result::IoError;
            }
            pipe.state = PipeState::Closed;
            return Result::Ok;
        }
        if (attempt + 1 < pollBudget) { io.delayUs(io.context, delayUs); }
    }
    poison(pipe, Transfer::Closed);
    return Result::TimedOut;
}

Transfer sendPacket(SmdPipe &pipe, const u8 *bytes, u32 size)
{
    if (pipe.state != PipeState::Ready) { return Transfer::Closed; }
    if (!bytes || !size || size > kMaximumPacketBytes || (size & 3)) { return Transfer::IoError; }
    u32 reader = 0, writer = 0;
    if (!ready(pipe) || !indices(pipe, 0, &reader, &writer)) { return poison(pipe, Transfer::Closed); }
    if (!pipe.io.barrier(pipe.io.context)) { return poison(pipe, Transfer::IoError); }
    const u32 mask = pipe.fifoBytes - 1;
    const u32 freeBytes = (reader - writer - 4) & mask;
    const u32 total = kPacketHeaderBytes + size;
    if (freeBytes < total) { return Transfer::WouldBlock; }
    u32 cursor = writer;
    for (u32 index = 0; index < total; index += 4) {
        const u32 word = index == 0 ? size : index < kPacketHeaderBytes ? 0 :
            readWord(bytes + index - kPacketHeaderBytes);
        if (!write(pipe, SmdSpace::Transmit, cursor, word)) { return poison(pipe, Transfer::IoError); }
        cursor = (cursor + 4) & mask;
    }
    if (!pipe.io.barrier(pipe.io.context) ||
        !write(pipe, SmdSpace::Control, kHalfBytes + kDataRead, 0) ||
        !write(pipe, SmdSpace::Control, kWriteIndex, cursor) ||
        !pipe.io.barrier(pipe.io.context) ||
        !write(pipe, SmdSpace::Control, kDataWritten, 1) ||
        !write(pipe, SmdSpace::Control, kMaskReceive, 0) ||
        !pipe.io.barrier(pipe.io.context) || !pipe.io.notify(pipe.io.context)) {
        return poison(pipe, Transfer::IoError);
    }
    return Transfer::Ok;
}

Transfer receivePacket(SmdPipe &pipe, u8 *bytes, u32 capacity, u32 *size)
{
    if (size) { *size = 0; }
    if (pipe.state != PipeState::Ready) { return Transfer::Closed; }
    if (!bytes || !size) { return Transfer::IoError; }
    u32 reader = 0, writer = 0;
    if (!ready(pipe) || !indices(pipe, kHalfBytes, &reader, &writer)) { return poison(pipe, Transfer::Closed); }
    if (!pipe.io.barrier(pipe.io.context)) { return poison(pipe, Transfer::IoError); }
    const u32 mask = pipe.fifoBytes - 1;
    const u32 available = (writer - reader) & mask;
    if (available < kPacketHeaderBytes) { return Transfer::WouldBlock; }
    u32 packetBytes = 0;
    if (!read(pipe, SmdSpace::Receive, reader, &packetBytes)) { return poison(pipe, Transfer::IoError); }
    if (packetBytes < 20 || packetBytes > kMaximumPacketBytes ||
        packetBytes > capacity || (packetBytes & 3)) { return poison(pipe, Transfer::IoError); }
    const u32 total = kPacketHeaderBytes + packetBytes;
    if (available < total) { return Transfer::WouldBlock; }
    if (!pipe.io.barrier(pipe.io.context)) { return poison(pipe, Transfer::IoError); }
    u32 cursor = (reader + kPacketHeaderBytes) & mask;
    for (u32 index = 0; index < packetBytes; index += 4) {
        u32 word = 0;
        if (!read(pipe, SmdSpace::Receive, cursor, &word)) { return poison(pipe, Transfer::IoError); }
        writeWord(bytes + index, word);
        cursor = (cursor + 4) & mask;
    }
    if (!pipe.io.barrier(pipe.io.context) ||
        !write(pipe, SmdSpace::Control, kHalfBytes + kReadIndex, cursor) ||
        !pipe.io.barrier(pipe.io.context) ||
        !write(pipe, SmdSpace::Control, kHalfBytes + kDataWritten, 0) ||
        !write(pipe, SmdSpace::Control, kDataRead, 1) ||
        !write(pipe, SmdSpace::Control, kMaskReceive, 1) ||
        !pipe.io.barrier(pipe.io.context) || !pipe.io.notify(pipe.io.context)) {
        return poison(pipe, Transfer::IoError);
    }
    *size = packetBytes;
    return Transfer::Ok;
}

Transport pipeTransport(SmdPipe &pipe)
{
    return {&pipe, transportSend, transportReceive, transportDelay};
}

}
}
