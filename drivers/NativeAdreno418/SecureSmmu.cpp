#include "SecureSmmu.h"

namespace adreno {
namespace secure {
namespace {

constexpr u8 kMmu3[] = {
    0x5b,0x82,0x4b,0x0d,0x4d,0x4d,0x55,0x33,0x08,0x5f,0x48,0x49,0x44,0x0d,0x51,0x43,
    0x4f,0x4d,0x32,0x34,0x30,0x30,0x00,0x08,0x5f,0x55,0x49,0x44,0x0a,0x03,0x08,0x5f,
    0x44,0x45,0x50,0x12,0x06,0x01,0x50,0x45,0x50,0x30,0x14,0x3d,0x5f,0x43,0x52,0x53,
    0x00,0xa4,0x11,0x35,0x0a,0x32,0x86,0x09,0x00,0x01,0x00,0x00,0xb1,0xfd,0x00,0xb0,
    0x00,0x00,0x89,0x06,0x00,0x03,0x01,0x46,0x00,0x00,0x00,0x89,0x06,0x00,0x03,0x01,
    0x10,0x01,0x00,0x00,0x89,0x06,0x00,0x03,0x01,0x11,0x01,0x00,0x00,0x89,0x06,0x00,
    0x03,0x01,0x12,0x01,0x00,0x00,0x79,0x00,0x14,0x1b,0x47,0x55,0x49,0x44,0x00,0xa4,
    0x11,0x13,0x0a,0x10,0x12,0xc7,0x33,0x98,0x92,0x32,0xfb,0x4f,0xb0,0xf4,0x2b,0xd2,
    0x0e,0x1f,0x7f,0x66,0x14,0x0f,0x43,0x54,0x58,0x49,0x00,0xa4,0x11,0x07,0x0a,0x04,
    0x03,0x00,0x00,0x01,0x14,0x11,0x4f,0x46,0x46,0x49,0x00,0xa4,0x11,0x09,0x0a,0x06,
    0x00,0x01,0x02,0x03,0x04,0x08,0x14,0x21,0x53,0x32,0x43,0x42,0x00,0xa4,0x12,0x19,
    0x03,0x12,0x06,0x04,0x00,0x00,0x00,0x00,0x12,0x06,0x04,0x01,0x01,0x00,0x00,0x12,
    0x08,0x04,0x01,0x0a,0x02,0x00,0x0a,0x02,0x14,0x14,0x58,0x50,0x55,0x45,0x00,0x08,
    0x42,0x55,0x46,0x46,0x11,0x03,0x01,0x01,0xa4,0x42,0x55,0x46,0x46
};

constexpr u8 kMmu3Handset[] = {
    0x5b,0x82,0x4f,0x0c,0x4d,0x4d,0x55,0x33,0x08,0x5f,0x48,0x49,0x44,0x0d,0x51,0x43,
    0x4f,0x4d,0x32,0x34,0x30,0x30,0x00,0x08,0x5f,0x55,0x49,0x44,0x0a,0x03,0x14,0x3d,
    0x5f,0x43,0x52,0x53,0x00,0xa4,0x11,0x35,0x0a,0x32,0x86,0x09,0x00,0x01,0x00,0x00,
    0xb1,0xfd,0x00,0xb0,0x00,0x00,0x89,0x06,0x00,0x03,0x01,0x46,0x00,0x00,0x00,0x89,
    0x06,0x00,0x03,0x01,0x10,0x01,0x00,0x00,0x89,0x06,0x00,0x03,0x01,0x11,0x01,0x00,
    0x00,0x89,0x06,0x00,0x03,0x01,0x12,0x01,0x00,0x00,0x79,0x00,0x14,0x1b,0x47,0x55,
    0x49,0x44,0x00,0xa4,0x11,0x13,0x0a,0x10,0x12,0xc7,0x33,0x98,0x92,0x32,0xfb,0x4f,
    0xb0,0xf4,0x2b,0xd2,0x0e,0x1f,0x7f,0x66,0x14,0x0f,0x43,0x54,0x58,0x49,0x00,0xa4,
    0x11,0x07,0x0a,0x04,0x03,0x00,0x00,0x01,0x14,0x11,0x4f,0x46,0x46,0x49,0x00,0xa4,
    0x11,0x09,0x0a,0x06,0x00,0x01,0x02,0x03,0x04,0x08,0x14,0x21,0x53,0x32,0x43,0x42,
    0x00,0xa4,0x12,0x19,0x03,0x12,0x06,0x04,0x00,0x00,0x00,0x00,0x12,0x06,0x04,0x01,
    0x01,0x00,0x00,0x12,0x08,0x04,0x01,0x0a,0x02,0x00,0x0a,0x02,0x14,0x14,0x58,0x50,
    0x55,0x45,0x00,0x08,0x42,0x55,0x46,0x46,0x11,0x03,0x01,0x01,0xa4,0x42,0x55,0x46,
    0x46
};

bool matchesDevice(const u8 *table, u32 bytes, const u8 *profile, u32 profileBytes)
{
    if (bytes < profileBytes) { return false; }
    for (u32 index = 0; index < profileBytes; ++index) {
        if (table[index] != profile[index]) { return false; }
    }
    return true;
}

bool validContract(const Contract &contract)
{
    return contract.platformQualified && contract.firmwareExited && contract.exclusiveColdGpu &&
        contract.acpiProfileQualified && contract.restorePolicySelected;
}

Result call(Configuration &configuration, MonitorRegisters registers, u32 callBudget)
{
    const MonitorRegisters original = registers;
    for (;;) {
        if (configuration.calls >= callBudget) { return Result::ResumeLimit; }
        ++configuration.calls;
        if (!configuration.io.invoke(configuration.io.context, &registers)) { return Result::CallFault; }
        configuration.returned = registers;
        if (registers.values[0] == 1) {
            ++configuration.resumes;
            const u64 cookie = registers.values[6];
            registers = original;
            registers.values[0] = 1;
            registers.values[6] = cookie;
            continue;
        }
        if (registers.values[0] == 0xffffffffu || registers.values[0] == ~0ull) { return Result::Unsupported; }
        return registers.values[0] == 0 ? Result::Ok : Result::MonitorError;
    }
}

bool read(Contexts &contexts, const RegisterIo &io, u32 offset, u32 &value)
{
    contexts.lastOffset = offset;
    contexts.lastReadSucceeded = false;
    contexts.lastValue = 0;
    ++contexts.reads;
    u32 observed = 0;
    if (!io.read(io.context, RegisterSpace::Smmu, offset, &observed) || observed == 0xffffffffu) {
        return false;
    }
    value = observed;
    contexts.lastValue = observed;
    contexts.lastReadSucceeded = true;
    ++contexts.successfulReads;
    return true;
}

}

AcpiProfile identifyLumiaSmmuProfile(const u8 *table, u32 bytes)
{
    if (!table || bytes < 36 + sizeof(kMmu3Handset) || bytes > 1024 * 1024 ||
        table[0] != 'D' || table[1] != 'S' || table[2] != 'D' || table[3] != 'T') {
        return AcpiProfile::Unsupported;
    }
    u32 declared = 0;
    for (u32 index = 0; index < 4; ++index) { declared |= static_cast<u32>(table[4 + index]) << (index * 8); }
    if (declared != bytes) { return AcpiProfile::Unsupported; }
    u8 checksum = 0;
    for (u32 index = 0; index < bytes; ++index) { checksum = static_cast<u8>(checksum + table[index]); }
    if (checksum) { return AcpiProfile::Unsupported; }
    AcpiProfile qualified = AcpiProfile::Unsupported;
    for (u32 offset = 36; offset <= bytes - sizeof(kMmu3Handset); ++offset) {
        AcpiProfile profile = AcpiProfile::Unsupported;
        if (matchesDevice(table + offset, bytes - offset, kMmu3, sizeof(kMmu3))) {
            profile = AcpiProfile::PinnedWithDependency;
        } else if (matchesDevice(table + offset, bytes - offset, kMmu3Handset, sizeof(kMmu3Handset))) {
            profile = AcpiProfile::HandsetWithoutDependency;
        }
        if (profile == AcpiProfile::Unsupported) { continue; }
        if (qualified != AcpiProfile::Unsupported) { return AcpiProfile::Unsupported; }
        qualified = profile;
    }
    return qualified;
}

bool isLumiaSmmuProfile(const u8 *table, u32 bytes)
{
    return identifyLumiaSmmuProfile(table, bytes) != AcpiProfile::Unsupported;
}

Result restoreGraphicsConfiguration(Configuration &configuration, const MonitorIo &io,
    const Contract &contract, u32 callBudget)
{
    if (configuration.attempted) { return Result::NotReady; }
    if (!validContract(contract)) { return Result::Unqualified; }
    if (!io.invoke || callBudget < 2 || callBudget > 64) { return Result::Invalid; }
    configuration.io = io;
    configuration.contract = contract;
    configuration.attempted = true;
    MonitorRegisters registers = {};
    registers.values[0] = kCallAvailable;
    registers.values[1] = 1;
    registers.values[2] = kRestoreConfiguration;
    Result result = call(configuration, registers, callBudget);
    if (result != Result::Ok) { return result; }
    if (configuration.returned.values[1] != 1) { return Result::Unsupported; }
    configuration.availability = 1;
    if (configuration.calls >= callBudget) { return Result::ResumeLimit; }
    registers = {};
    registers.values[0] = kRestoreConfiguration;
    registers.values[1] = 2;
    registers.values[2] = kGraphicsDevice;
    configuration.restoreAttempted = true;
    result = call(configuration, registers, callBudget);
    if (result != Result::Ok) { return result; }
    if (configuration.returned.values[1] != 0) { return Result::ServiceError; }
    configuration.restored = true;
    return Result::Ok;
}

Result inspectGraphicsContexts(Contexts &contexts, const RegisterIo &io,
    const Configuration &configuration)
{
    contexts = {};
    if (!configuration.attempted || !configuration.restoreAttempted || !configuration.restored ||
        configuration.availability != 1 || configuration.calls < 2 ||
        configuration.returned.values[0] || configuration.returned.values[1] ||
        !validContract(configuration.contract)) { return Result::Unqualified; }
    if (!io.read) { return Result::Invalid; }
    GpuSnapshot gpu = {};
    if (readSnapshot(io, &gpu) != ProbeResult::Ready || (gpu.status & kGpuBusy) ||
        (gpu.meControl & kMeHalt) == 0 || (gpu.interruptStatus & kGpuFaultMask)) { return Result::NotReady; }
    Result result = Result::Ok;
    for (u32 index = 0; index < kContextCount; ++index) {
        ContextSnapshot &bank = contexts.banks[index];
        const u32 offsets[] = {0, 4, 0x34, 0x20, 0x24, 0x30, 0x38, 0x58, 0x7f4};
        u32 *fields[] = {&bank.control, &bank.auxiliary, &bank.contextIdentifier,
            &bank.tableBase[0], &bank.tableBase[1], &bank.translationControl,
            &bank.memoryAttributes, &bank.fault, &bank.tlbStatus};
        for (u32 field = 0; field < sizeof(offsets) / sizeof(offsets[0]); ++field) {
            if (!read(contexts, io, kContextOffset + index * 4096 + offsets[field], *fields[field])) {
                return Result::IoError;
            }
            bank.validFields |= 1u << field;
        }
        if (bank.fault) { result = Result::ContextFault; }
        else if ((bank.control & 1) && result == Result::Ok) { result = Result::ContextEnabled; }
        else if ((bank.tlbStatus & 1) && result == Result::Ok) { result = Result::NotReady; }
    }
    contexts.accessibleDisabled = result == Result::Ok;
    return result;
}

const char *resultName(Result result)
{
    switch (result) {
    case Result::Ok: return "OK";
    case Result::Invalid: return "INVALID";
    case Result::Unqualified: return "UNQUALIFIED";
    case Result::NotReady: return "NOT READY";
    case Result::CallFault: return "SECURE CALL FAULT";
    case Result::Unsupported: return "UNSUPPORTED SECURE CALL";
    case Result::MonitorError: return "SECURE MONITOR ERROR";
    case Result::ServiceError: return "SECURE SERVICE ERROR";
    case Result::ResumeLimit: return "SECURE RESUME LIMIT";
    case Result::IoError: return "CONTEXT READ FAULT";
    case Result::ContextEnabled: return "CONTEXT ALREADY ENABLED";
    case Result::ContextFault: return "INHERITED CONTEXT FAULT";
    case Result::Mismatch: return "MISMATCH";
    }
    return "UNKNOWN";
}

}
}
