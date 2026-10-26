#ifndef NATIVE_ADRENO418_SMD_MEMORY_H
#define NATIVE_ADRENO418_SMD_MEMORY_H

#include "SmdPipe.h"

namespace adreno {
namespace rpm {

constexpr u64 kMessageRamPhysical = 0xfc428000;
constexpr u32 kMessageRamBytes = 0x8000;
constexpr u64 kRpmDoorbellPhysical = 0xf900d008;

struct MemoryRegion { u64 physical; u64 address; u32 bytes; };
struct MemoryLayout { MemoryRegion control; MemoryRegion fifo; MemoryRegion doorbell; };
struct MemoryAccess {
    void *context;
    bool (*translate)(void *, u64 address, bool writing, u64 *par);
    bool (*read32)(void *, u64 address, u32 *value);
    bool (*write32)(void *, u64 address, u32 value);
    bool (*barrier)(void *);
    void (*delayUs)(void *, u32);
};
enum class MemoryState : u32 { Empty, Observed, Owned, Retired, Poisoned };
struct MemoryBinding {
    MemoryLayout layout;
    MemoryAccess access;
    MemoryState state;
};
struct ChannelSnapshot { u32 words[22]; };

Result observeMemory(MemoryBinding &binding, const MemoryLayout &layout,
                     const MemoryAccess &access);
Result snapshotChannel(MemoryBinding &binding, ChannelSnapshot &snapshot);
bool closedAndEmpty(const ChannelSnapshot &snapshot, u32 fifoBytes);
Result claimMemory(MemoryBinding &binding, bool firmwareExited, bool exclusiveOwner);
Result retireMemory(MemoryBinding &binding, const SmdPipe &pipe);
SmdIo memoryIo(MemoryBinding &binding);

}
}

#endif
