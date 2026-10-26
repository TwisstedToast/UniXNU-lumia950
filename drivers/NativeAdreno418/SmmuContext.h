#ifndef NATIVE_ADRENO418_SMMU_CONTEXT_H
#define NATIVE_ADRENO418_SMMU_CONTEXT_H
#include "GpuIo.h"
#include "LpaeCore.h"

namespace adreno {

enum class ContextResult : u32 {
    Ok, Invalid, NotReady, AlreadyEnabled, IoError, TimedOut, TranslationFault, Mismatch
};

struct ContextIo {
    RegisterIo registers;
    bool (*cleanTables)(void *, const lpae::Tables &);
};

struct ContextBank {
    ContextIo io;
    u32 offset;
    u32 asid;
    u32 fault;
    bool ownsBank;
    bool enabled;
};

ContextResult bindDisabledContext(ContextBank &bank, const ContextIo &io,
    const lpae::Tables &tables, u32 index, u32 asid, bool exclusiveOwnership);
ContextResult synchronizeContext(ContextBank &bank, u32 pollBudget);
ContextResult checkTranslation(ContextBank &bank, u32 iova, u32 physical, u32 pollBudget);
ContextResult disableContext(ContextBank &bank, u32 pollBudget);

}
#endif
