#ifndef NATIVE_ADRENO418_SECURE_SMMU_H
#define NATIVE_ADRENO418_SECURE_SMMU_H

#include "GpuIo.h"

namespace adreno {
namespace secure {

constexpr u32 kCallAvailable = 0x02000601;
constexpr u32 kRestoreConfiguration = 0x02000c02;
constexpr u32 kGraphicsDevice = 18;
constexpr u32 kContextOffset = 0x8000;
constexpr u32 kContextCount = 2;

enum class AcpiProfile : u32 { Unsupported, PinnedWithDependency, HandsetWithoutDependency };

struct MonitorRegisters { u64 values[8]; };
struct MonitorIo {
    void *context;
    bool (*invoke)(void *, MonitorRegisters *);
};
struct Contract {
    bool platformQualified;
    bool firmwareExited;
    bool exclusiveColdGpu;
    bool acpiProfileQualified;
    bool restorePolicySelected;
};
enum class Result : u32 {
    Ok, Invalid, Unqualified, NotReady, CallFault, Unsupported, MonitorError,
    ServiceError, ResumeLimit, IoError, ContextEnabled, ContextFault, Mismatch
};
struct Configuration {
    MonitorIo io;
    Contract contract;
    MonitorRegisters returned;
    u32 availability;
    u32 calls;
    u32 resumes;
    bool attempted;
    bool restoreAttempted;
    bool restored;
};
struct ContextSnapshot {
    u32 control;
    u32 auxiliary;
    u32 contextIdentifier;
    u32 tableBase[2];
    u32 translationControl;
    u32 memoryAttributes;
    u32 fault;
    u32 tlbStatus;
    u32 validFields;
};
struct Contexts {
    ContextSnapshot banks[kContextCount];
    u32 lastOffset;
    u32 lastValue;
    u32 reads;
    u32 successfulReads;
    bool lastReadSucceeded;
    bool accessibleDisabled;
};

bool isLumiaSmmuProfile(const u8 *table, u32 bytes);
AcpiProfile identifyLumiaSmmuProfile(const u8 *table, u32 bytes);
Result restoreGraphicsConfiguration(Configuration &configuration, const MonitorIo &io,
    const Contract &contract, u32 callBudget);
Result inspectGraphicsContexts(Contexts &contexts, const RegisterIo &io,
    const Configuration &configuration);
const char *resultName(Result result);

}
}
#endif
