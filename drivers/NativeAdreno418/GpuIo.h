#ifndef NATIVE_ADRENO418_GPU_IO_H
#define NATIVE_ADRENO418_GPU_IO_H

#include "AdrenoTypes.h"

namespace adreno {

enum class RegisterSpace : u32 { Mmss, Gpu, Smmu };

struct RegisterIo {
    void *context;
    bool (*read)(void *, RegisterSpace, u32 byteOffset, u32 *value);
    bool (*write)(void *, RegisterSpace, u32 byteOffset, u32 value);
    void (*barrier)(void *);
    void (*delayUs)(void *, u32 microseconds);
};

struct PowerSnapshot {
    u32 gx;
    u32 cx;
    u32 coreClock;
    u32 ahbClock;
    u32 timerClock;
};

enum class ProbeResult : u32 {
    Ready, IoError, DomainOff, CollapsePending, ClockOff, NoResponse
};

struct GpuSnapshot {
    PowerSnapshot power;
    u32 version;
    u32 configuration;
    u32 status;
    u32 interruptStatus;
    u32 ringBase;
    u32 ringControl;
    u32 readPointer;
    u32 writePointer;
    u32 meControl;
};

constexpr u32 kCoreClockOffset = 0x4028;
constexpr u32 kAhbClockOffset = 0x403c;
constexpr u32 kTimerClockOffset = 0x40b0;
constexpr u32 kBranchOff = 1u << 31;
constexpr u32 kBranchEnable = 1;
constexpr u32 kInterruptStatusIndex = 0x017d;
constexpr u32 kGpuBusy = 1u << 31;
constexpr u32 kMeHalt = 1u << 28;
constexpr u32 kGpuFaultMask = 0x03210f7eu;

ProbeResult readPower(const RegisterIo &io, PowerSnapshot *snapshot);
ProbeResult checkPower(const PowerSnapshot &snapshot);
ProbeResult readSnapshot(const RegisterIo &io, GpuSnapshot *snapshot);
const char *probeResultName(ProbeResult result);

}
#endif
