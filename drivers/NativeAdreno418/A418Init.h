#ifndef NATIVE_ADRENO418_A418_INIT_H
#define NATIVE_ADRENO418_A418_INIT_H

#include "GpuIo.h"

namespace adreno {

struct HardwareContract {
    u32 chipId;
    bool identityQualified;
    bool exclusiveOwnership;
};
enum class HardwareState : u32 { Empty, Programming, Ready, Stopping, Stopped, Poisoned };
enum class HardwareResult : u32 { Ok, Invalid, Unqualified, NotReady, IoError, TimedOut, Mismatch };
struct CommandHardware {
    RegisterIo io;
    HardwareState state;
    u32 lastIndex;
    u32 lastValue;
    u32 polls;
    u32 version;
};

bool validHardwareContract(const HardwareContract &contract);
HardwareResult initializeCommandHardware(CommandHardware &hardware, const RegisterIo &io,
    const HardwareContract &contract, u32 pollBudget);
HardwareResult stopCommandHardware(CommandHardware &hardware, bool translationsDisabled,
    u32 pollBudget);

}
#endif
