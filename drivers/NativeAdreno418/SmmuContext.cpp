#include "SmmuContext.h"
#include "BoardConstants.h"

namespace adreno {
namespace {
constexpr u32 kSctlr = 0;
constexpr u32 kTcr2 = 0x10;
constexpr u32 kTtbr0 = 0x20;
constexpr u32 kTtbr1 = 0x28;
constexpr u32 kTcr = 0x30;
constexpr u32 kMair0 = 0x38;
constexpr u32 kMair1 = 0x3c;
constexpr u32 kFsr = 0x58;
constexpr u32 kPar = 0x50;
constexpr u32 kTlbiAsid = 0x610;
constexpr u32 kTlbSync = 0x7f0;
constexpr u32 kTlbStatus = 0x7f4;
constexpr u32 kAts1Pr = 0x800;
constexpr u32 kAtsr = 0x8f0;
constexpr u32 kControl = (1u << 12) | (1u << 7) | (1u << 5) | 7u;

bool read(ContextBank &bank, u32 offset, u32 *value)
{
    return bank.io.registers.read(bank.io.registers.context, RegisterSpace::Smmu,
                                   bank.offset + offset, value);
}
bool write(ContextBank &bank, u32 offset, u32 value)
{
    return bank.io.registers.write(bank.io.registers.context, RegisterSpace::Smmu,
                                    bank.offset + offset, value);
}
bool write64(ContextBank &bank, u32 offset, u64 value)
{
    return write(bank, offset + 4, static_cast<u32>(value >> 32)) &&
        write(bank, offset, static_cast<u32>(value));
}
ContextResult waitClear(ContextBank &bank, u32 offset, u32 pollBudget)
{
    for (u32 poll = 0; poll < pollBudget; poll++) {
        u32 value = 0;
        if (!read(bank, offset, &value)) { return ContextResult::IoError; }
        if (value == 0xffffffffu) { return ContextResult::IoError; }
        if ((value & 1) == 0) {
            bank.io.registers.barrier(bank.io.registers.context);
            return ContextResult::Ok;
        }
        bank.io.registers.delayUs(bank.io.registers.context, 1);
    }
    return ContextResult::TimedOut;
}
}

ContextResult synchronizeContext(ContextBank &bank, u32 pollBudget)
{
    if (!bank.ownsBank || pollBudget == 0 || pollBudget > 100000) { return ContextResult::Invalid; }
    bank.io.registers.barrier(bank.io.registers.context);
    if (!write(bank, kTlbiAsid, bank.asid) || !write(bank, kTlbSync, 0)) {
        return ContextResult::IoError;
    }
    return waitClear(bank, kTlbStatus, pollBudget);
}

ContextResult bindDisabledContext(ContextBank &bank, const ContextIo &io,
    const lpae::Tables &tables, u32 index, u32 asid, bool exclusiveOwnership, u32 pollBudget)
{
    if (bank.ownsBank || !exclusiveOwnership || index > 1 || asid == 0 || asid > 255 ||
        !pollBudget || pollBudget > 100000 ||
        !io.registers.read || !io.registers.write || !io.registers.barrier ||
        !io.registers.delayUs || !io.cleanTables || !lpae::validTables(tables)) {
        return ContextResult::Invalid;
    }
    GpuSnapshot snapshot = {};
    if (readSnapshot(io.registers, &snapshot) != ProbeResult::Ready ||
        (snapshot.status & kGpuBusy) != 0 || (snapshot.meControl & kMeHalt) == 0) {
        return ContextResult::NotReady;
    }
    u32 control = 0;
    const u32 offset = 0x8000 + index * 0x1000;
    if (!io.registers.read(io.registers.context, RegisterSpace::Smmu, offset, &control) ||
        control == 0xffffffffu) { return ContextResult::IoError; }
    if (control & 1) { return ContextResult::AlreadyEnabled; }
    u32 fault = 0;
    if (!io.registers.read(io.registers.context, RegisterSpace::Smmu, offset + kFsr, &fault)) {
        return ContextResult::IoError;
    }
    if (fault != 0) { return ContextResult::TranslationFault; }
    if (!io.cleanTables(io.registers.context, tables)) { return ContextResult::IoError; }
    bank = {io, offset, asid, 0, true, false};
    if (!write(bank, kSctlr, 0) ||
        !write64(bank, kTtbr0, tables.rootPhysical | (static_cast<u64>(asid) << 48)) ||
        !write64(bank, kTtbr1, 0) || !write(bank, kTcr2, 7u << 15) ||
        !write(bank, kTcr, lpae::kTcr | (2u << 12)) ||
        !write(bank, kMair0, lpae::kMair0) || !write(bank, kMair1, 0)) {
        return ContextResult::IoError;
    }
    const ContextResult synced = synchronizeContext(bank, pollBudget);
    if (synced != ContextResult::Ok) { return synced; }
    io.registers.barrier(io.registers.context);
    if (!write(bank, kSctlr, kControl)) { return ContextResult::IoError; }
    io.registers.barrier(io.registers.context);
    if (!read(bank, kSctlr, &control) || control != kControl) { return ContextResult::Mismatch; }
    bank.enabled = true;
    return ContextResult::Ok;
}

ContextResult checkTranslation(ContextBank &bank, u32 iova, u32 physical, u32 pollBudget)
{
    return checkTranslationAccess(bank, iova, physical, ContextAccess::PrivilegedRead, pollBudget);
}

ContextResult checkTranslationAccess(ContextBank &bank, u32 iova, u32 physical,
    ContextAccess access, u32 pollBudget)
{
    if (!bank.ownsBank || !bank.enabled || pollBudget == 0 || pollBudget > 100000 ||
        static_cast<u32>(access) > 3 ||
        (iova & 4095) != (physical & 4095)) { return ContextResult::Invalid; }
    if (!write64(bank, kAts1Pr + static_cast<u32>(access) * 8, iova)) { return ContextResult::IoError; }
    bank.io.registers.barrier(bank.io.registers.context);
    const ContextResult waited = waitClear(bank, kAtsr, pollBudget);
    if (waited != ContextResult::Ok) { return waited; }
    u32 low = 0;
    u32 high = 0;
    if (!read(bank, kPar, &low) || !read(bank, kPar + 4, &high)) { return ContextResult::IoError; }
    if (low & 1) {
        bank.fault = low;
        return ContextResult::TranslationFault;
    }
    if ((high & 0x0000ffffu) != 0 || (low & 0xfffff000) != (physical & 0xfffff000)) {
        return ContextResult::Mismatch;
    }
    u32 fsr = 0;
    if (!read(bank, kFsr, &fsr)) { return ContextResult::IoError; }
    if (fsr != 0) { bank.fault = fsr; return ContextResult::TranslationFault; }
    return ContextResult::Ok;
}

ContextResult disableContext(ContextBank &bank, u32 pollBudget)
{
    if (!bank.ownsBank || pollBudget == 0 || pollBudget > 100000) { return ContextResult::Invalid; }
    GpuSnapshot snapshot = {};
    if (readSnapshot(bank.io.registers, &snapshot) != ProbeResult::Ready ||
        (snapshot.status & kGpuBusy) != 0 || (snapshot.meControl & kMeHalt) == 0) {
        return ContextResult::NotReady;
    }
    if (!write(bank, kSctlr, 0)) { return ContextResult::IoError; }
    bank.io.registers.barrier(bank.io.registers.context);
    u32 control = 0;
    if (!read(bank, kSctlr, &control) || control != 0) { return ContextResult::Mismatch; }
    const ContextResult result = synchronizeContext(bank, pollBudget);
    if (result == ContextResult::Ok) {
        bank.enabled = false;
        bank.ownsBank = false;
    }
    return result;
}

}
