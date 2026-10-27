#ifndef NATIVE_ADRENO418_SPMI_CORE_H
#define NATIVE_ADRENO418_SPMI_CORE_H

#include "AdrenoTypes.h"
#include "RpmCore.h"

namespace adreno {
namespace spmi {

constexpr u64 kControllerPhysical = 0xfc4c0000;
constexpr u32 kVersionOffset = 0xf000;
constexpr u32 kCommandOffset = 0xf800;
constexpr u32 kConfigurationOffset = 0xf804;
constexpr u32 kStatusOffset = 0xf808;
constexpr u32 kWriteDataOffset = 0xf810;
constexpr u32 kReadDataOffset = 0xf818;

struct Io {
    void *context;
    bool (*read)(void *, u32, u32 *);
    bool (*write)(void *, u32, u32);
    void (*barrier)(void *);
    void (*delayUs)(void *, u32);
};
enum class State : u32 { Empty, Ready, InFlight, Poisoned };
enum class Result : u32 { Ok, Invalid, Unqualified, NotReady, IoError, TimedOut, Denied, Failed };
struct Bus {
    Io io;
    State state;
    u32 version;
    u32 status;
    u32 command;
    u32 transfers;
};
struct RailSnapshot {
    u8 type;
    u8 subtype;
    u8 status;
    u8 range;
    u8 voltage;
    u8 mode;
    u8 enable;
    u32 microvolts;
};
struct Rail {
    RailSnapshot initial;
    RailSnapshot final;
    bool ready;
    bool changed;
    bool pinned;
    u32 stableSamples;
};
struct ObservedRail {
    RailSnapshot initial;
    RailSnapshot final;
    bool ready;
    bool pinned;
    u32 samples;
    u32 stableSamples;
    u32 minimumMicrovolts;
    u32 maximumMicrovolts;
    bool rpmManaged;
};
struct RpmSupplyContract {
    rpm::Resource clock;
    u32 kilohertz;
    bool correlatedAck;
    bool exclusiveColdGpu;
    bool oemPolicySelected;
};

Result bind(Bus &bus, const Io &io, bool firmwareExited, bool exclusiveChannel);
Result readByte(Bus &bus, u8 slave, u16 address, u8 &value, u32 polls);
Result writeGraphicsByte(Bus &bus, u16 address, u8 value, u32 polls);
Result inspectGraphicsRail(Bus &bus, RailSnapshot &snapshot, u32 polls);
Result enableGraphicsRail(Bus &bus, Rail &rail, bool gpuCollapsed, u32 polls);
Result verifyGraphicsRail(Bus &bus, Rail &rail, u32 samples, u32 delayUs, u32 polls);
bool graphicsReadCommand(u32 offset, u32 command);
bool knownGraphicsPolicy(const RailSnapshot &snapshot);
Result awaitGraphicsRail(Bus &bus, ObservedRail &rail, u32 samples, u32 attempts, u32 delayUs, u32 polls);
bool validRpmSupplyContract(const RpmSupplyContract &contract);
Result awaitManagedGraphicsRail(Bus &bus, ObservedRail &rail, const RpmSupplyContract &contract,
                               u32 samples, u32 attempts, u32 delayUs, u32 polls);
Result recheckGraphicsRail(Bus &bus, ObservedRail &rail, u32 polls);
const char *resultName(Result result);

}
}
#endif
