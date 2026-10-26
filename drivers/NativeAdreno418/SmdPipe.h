#ifndef NATIVE_ADRENO418_SMD_PIPE_H
#define NATIVE_ADRENO418_SMD_PIPE_H

#include "RpmCore.h"

namespace adreno {
namespace rpm {

enum class SmdSpace : u32 { Control, Transmit, Receive };
enum class PipeState : u32 { Empty, Opening, Ready, Poisoned };
struct SmdIo {
    void *context;
    bool (*read32)(void *, SmdSpace, u32 offset, u32 *value);
    bool (*write32)(void *, SmdSpace, u32 offset, u32 value);
    bool (*barrier)(void *);
    bool (*notify)(void *);
    void (*delayUs)(void *, u32);
};
struct SmdPipe {
    SmdIo io;
    PipeState state;
    u32 fifoBytes;
};

Result adoptPipe(SmdPipe &pipe, const SmdIo &io, u32 controlBytes, u32 fifoBytes,
                 bool exclusiveOwnership, bool uncachedMappings);
Result openPipe(SmdPipe &pipe, const SmdIo &io, u32 controlBytes, u32 fifoBytes,
                bool exclusiveOwnership, bool uncachedMappings, u32 pollBudget, u32 delayUs);
Transfer sendPacket(SmdPipe &pipe, const u8 *bytes, u32 size);
Transfer receivePacket(SmdPipe &pipe, u8 *bytes, u32 capacity, u32 *size);
Transport pipeTransport(SmdPipe &pipe);

}
}

#endif
