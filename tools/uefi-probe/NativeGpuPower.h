const MemoryRegion nativeGpuRegions[] = {
    {lumia950::kMmssPhys, lumia950::kMmssPhys, 0x10000},
    {lumia950::kGpuPhys, lumia950::kGpuPhys, 0x20000},
    {spmi::kControllerPhysical, spmi::kControllerPhysical, 0x10000}
};

bool NativeGpuMappings(EFI_SYSTEM_TABLE *systemTable)
{
    const ProbeRpmGcd gcd = ProbeFindMmioGcd(systemTable, nativeGpuRegions);
    PrintHex("GPU_GCD_STATUS", gcd.status);
    if (!gcd.qualified) { return false; }
    for (const MemoryRegion &region : nativeGpuRegions) {
        for (u64 offset = 0; offset < region.bytes; offset += 4096) {
            for (u32 access = 0; access < 2; ++access) {
                u64 par = 0;
                if (!Translate(nullptr, region.address + offset, access != 0, &par) || (par & 1) ||
                    (par & 0x000ffffffffff000ull) != region.physical + offset ||
                    ((par >> 56) != 0 && (par >> 56) != 4)) {
                    PrintHex("GPU_BAD_MAPPING_ADDRESS", region.address + offset);
                    PrintHex("GPU_BAD_MAPPING_PAR", par);
                    return false;
                }
            }
        }
    }
    Print("GPU_MMSS_SMMU_SPMI_MAPPINGS=DEVICE_IDENTITY\r\n");
    return true;
}

#include "NativeGpuInterrupts.h"

struct NativeGpuContext { bool gpuAccessible; };

bool NativeGpuAddress(NativeGpuContext &context, RegisterSpace space, u32 offset, u64 &address)
{
    if ((offset & 3) || offset >= 0x10000) { return false; }
    if (space == RegisterSpace::Mmss) { address = lumia950::kMmssPhys + offset; return true; }
    if (!context.gpuAccessible) { return false; }
    if (space == RegisterSpace::Gpu) { address = lumia950::kGpuPhys + offset; return true; }
    if (space == RegisterSpace::Smmu && offset >= secure::kContextOffset &&
        offset < secure::kContextOffset + secure::kContextCount * 4096) {
        address = lumia950::kGpuSmmuPhys + offset;
        return true;
    }
    return false;
}

bool NativeGpuRead(void *opaque, RegisterSpace space, u32 offset, u32 *value)
{
    u64 address = 0;
    return opaque && NativeGpuAddress(*static_cast<NativeGpuContext *>(opaque), space, offset, address) &&
        NativeRead(nullptr, address, value);
}

bool NativeGpuWrite(void *opaque, RegisterSpace space, u32 offset, u32 value)
{
    u64 address = 0;
    return space != RegisterSpace::Smmu && opaque &&
        NativeGpuAddress(*static_cast<NativeGpuContext *>(opaque), space, offset, address) &&
        NativeWrite(nullptr, address, value);
}

void NativeGpuBarrier(void *) { Barrier(nullptr); }

bool NativeSpmiRead(void *, u32 offset, u32 *value)
{
    if (offset != spmi::kVersionOffset && offset != spmi::kConfigurationOffset &&
        offset != spmi::kStatusOffset && offset != spmi::kReadDataOffset) { return false; }
    return NativeRead(nullptr, spmi::kControllerPhysical + offset, value);
}

bool NativeSpmiWrite(void *, u32 offset, u32 value)
{
    if (!spmi::graphicsReadCommand(offset, value)) { return false; }
    return NativeWrite(nullptr, spmi::kControllerPhysical + offset, value);
}

struct NativeGpuResult {
    NativeGpuInterrupts interrupts;
    spmi::Bus bus;
    pinned::Handoff handoff;
    rpm::Session session;
    power::Controller power;
    power::Result powerResult;
    GpuSnapshot snapshot;
    ProbeResult identity;
    CommandHardware hardware;
    HardwareResult hardwareResult;
    secure::Configuration configuration;
    secure::Result secureResult;
    secure::Contexts contexts;
    secure::Result contextResult;
    bool baselineComplete;
    bool complete;
};

void NativeGpuProgress(const ProbeFramebuffer &framebuffer, const char *stage, u32 pauseMicroseconds = 750000)
{
    nativeGpuStage = stage;
    ++nativeGpuCheckpoint;
    const u32 color = ProbeFramebufferColor(framebuffer.pixel_format, 60, 170, 255);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, 640, 0);
    Line(framebuffer, 0, "GPU BRINGUP V11 - CPU STATUS ONLY", color);
    Line(framebuffer, 1, ProbeGpuModeName(nativeGpuMode), color);
    Line(framebuffer, 2, stage, color);
    Number(framebuffer, 3, "TRACE STEP", nativeGpuCheckpoint, color);
    Flush(framebuffer, 0, 640);
    if (pauseMicroseconds) { Delay(nullptr, pauseMicroseconds); }
}

struct NativeScmTrace {
    const ProbeFramebuffer *framebuffer;
    u32 entries;
    secure::Request activeRequest;
    bool resumePending;
};

bool NativeScmInvoke(void *opaque, secure::MonitorRegisters *values)
{
    if (!opaque || !values || !ProbeGpuAllowsSecure(nativeGpuMode) || !NativeGpuInterruptsReady()) { return false; }
    auto &trace = *static_cast<NativeScmTrace *>(opaque);
    const u64 function = values->values[0];
    const bool resuming = function == 1;
    if (resuming) {
        if (!trace.resumePending) { return false; }
    } else {
        if (trace.resumePending) { return false; }
        trace.activeRequest = secure::identifyRequest(*values);
        if (trace.activeRequest == secure::Request::Invalid) { return false; }
    }
    const auto &framebuffer = *trace.framebuffer;
    const u32 color = ProbeFramebufferColor(framebuffer.pixel_format, 60, 170, 255);
    ++trace.entries;
    if (!resuming) {
        NativeGpuProgress(framebuffer, secure::requestStage(trace.activeRequest, false), 0);
        Number(framebuffer, 5, "SCM FUNCTION", function, color);
        Number(framebuffer, 6, "SCM ARG INFO", values->values[1], color);
        Number(framebuffer, 7, "SCM ARG ZERO", values->values[2], color);
        Number(framebuffer, 8, "SCM ARG ONE", values->values[3], color);
        Number(framebuffer, 9, "SCM ENTRY", trace.entries, color);
        Number(framebuffer, 10, "NATIVE TIMER IRQ", nativeInterruptContext->runtime.timerInterrupts(), color);
        Number(framebuffer, 11, "NATIVE SGI IRQ", nativeInterruptContext->runtime.sgiInterrupts(), color);
        Number(framebuffer, 12, "EBS GICD CTLR", nativeInterruptContext->inheritedDistributor, color);
        Number(framebuffer, 13, "EBS GICC CTLR", nativeInterruptContext->inheritedCpu, color);
        Number(framebuffer, 14, "EBS GICC PMR", nativeInterruptContext->inheritedPriorityMask, color);
        Number(framebuffer, 15, "IRQ ENABLED", NativeIrqEnabled(nullptr), color);
        Flush(framebuffer, 0, 640);
        Delay(nullptr, 2000000);
    }
    if (!NativeGpuInterruptsReady()) { return false; }
    ++nativeInterruptContext->monitorEntries;
    const bool returned = NativeGuardedScmCall(values);
    if (!NativeGpuInterruptsReady()) { return false; }
    trace.resumePending = ProbeGpuScmResumePending(returned, values->values[0]);
    if (trace.resumePending) { return true; }
    NativeGpuProgress(framebuffer, !returned ? "SCM TRAP RECOVERED" :
        secure::requestStage(trace.activeRequest, true), 0);
    Number(framebuffer, 5, "SCM MONITOR", values->values[0], color);
    Number(framebuffer, 6, "SCM SERVICE", values->values[1], color);
    Number(framebuffer, 7, "SCM COOKIE", values->values[6], color);
    Number(framebuffer, 8, "GUARD RETURNED", returned, color);
    Number(framebuffer, 9, "SCM ENTRY", trace.entries, color);
    Flush(framebuffer, 0, 440);
    Delay(nullptr, 2000000);
    return returned;
}

[[noreturn]] void NativeGpuIrqFailureScreen(const ProbeFramebuffer &framebuffer, const NativeGpuResult &result)
{
    const auto &context = result.interrupts;
    const auto &controller = context.runtime.controller();
    const auto failure = controller.failure();
    const u32 color = ProbeFramebufferColor(framebuffer.pixel_format, 255, 100, 60);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, framebuffer.height, 0);
    Line(framebuffer, 0, "GPU INITIALIZED - NATIVE IRQ BLOCKED", color);
    Line(framebuffer, 1, "V11 - NO WINDOWS OR UEFI SERVICES", color);
    Line(framebuffer, 2, nativeGpuStage, color);
    Number(framebuffer, 3, "IRQ RESULT", static_cast<u32>(context.result), color);
    Number(framebuffer, 4, "GIC ERROR", static_cast<u32>(controller.error()), color);
    Number(framebuffer, 5, "CHECK KIND", static_cast<u32>(failure.check), color);
    Number(framebuffer, 6, "REG SPACE D/C", static_cast<u32>(failure.space), color);
    Number(framebuffer, 7, "REG OFFSET", failure.offset, color);
    Number(framebuffer, 8, "EXPECTED/MIN", failure.expected, color);
    Number(framebuffer, 9, "OBSERVED", failure.observed, color);
    Number(framebuffer, 10, "COMPARE MASK", failure.mask, color);
    Number(framebuffer, 11, "DIST PRI MASK", controller.priorityBits(), color);
    Number(framebuffer, 12, "CPU PRI MASK", controller.priorityMaskBits(), color);
    Number(framebuffer, 13, "ID SOURCE", static_cast<u32>(controller.identity()), color);
    Number(framebuffer, 14, "EBS DIST CTLR", context.inheritedDistributor, color);
    Number(framebuffer, 15, "EBS CPU CTLR", context.inheritedCpu, color);
    Number(framebuffer, 16, "EBS CPU PMR", context.inheritedPriorityMask, color);
    for (u32 index = 0; index < nativeInterruptRegisterCount; ++index) {
        Number(framebuffer, 17 + index, nativeInterruptRegisters[index].label, context.snapshot[index], color);
    }
    const u32 tail = 17 + nativeInterruptRegisterCount;
    Number(framebuffer, tail, "TIMER IRQ", context.runtime.timerInterrupts(), color);
    Number(framebuffer, tail + 1, "SELF SGI IRQ", context.runtime.sgiInterrupts(), color);
    Number(framebuffer, tail + 2, "UNEXPECTED IRQ", context.runtime.unexpectedInterrupts(), color);
    Number(framebuffer, tail + 3, "LAST IRQ", context.runtime.lastInterrupt(), color);
    Number(framebuffer, tail + 4, "SCM ENTRIES", context.monitorEntries, color);
    Number(framebuffer, tail + 5, "MMIO FAULT", context.mmioFault, color);
    Number(framebuffer, tail + 6, "ABORT ESR", native_mmio_esr, color);
    Number(framebuffer, tail + 7, "ABORT FAR", native_mmio_far, color);
    Number(framebuffer, tail + 8, "ABORT ELR", native_mmio_elr, color);
    Line(framebuffer, tail + 9, "NO GPU FRAME - PINNED - FORCE REBOOT", color);
    Flush(framebuffer, 0, framebuffer.height);
    for (;;) { asm volatile("wfi"); }
}

[[noreturn]] void NativeGpuResultScreen(const ProbeFramebuffer &framebuffer, const NativeGpuResult &result)
{
    asm volatile("msr daifset, #15" ::: "memory");
    if (nativeInterruptContext) {
        NativeGpuCaptureInterrupts(*nativeInterruptContext);
        nativeInterruptContext->runtime.stop();
    }
    if (result.hardwareResult == HardwareResult::Ok && !result.complete && !result.baselineComplete &&
        result.interrupts.result != gicv2::RuntimeResult::Ok) NativeGpuIrqFailureScreen(framebuffer, result);
    const auto &handoff = result.handoff;
    const bool passed = result.complete || result.baselineComplete;
    const u32 color = ProbeFramebufferColor(framebuffer.pixel_format,
        passed ? 40 : 255, passed ? 255 : 100, 60);
    NativeGpuContext context = {};
    PowerSnapshot observed = {};
    const RegisterIo registers = {&context, NativeGpuRead, NativeGpuWrite, NativeGpuBarrier, Delay};
    readPower(registers, &observed);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, framebuffer.height, 0);
    const char *heading = result.baselineComplete ? "GPU HARDWARE BASELINE OK - NO SMC" :
        result.complete ? "NATIVE GPU + SMMU CONTEXT ACCESS OK" :
        result.hardwareResult == HardwareResult::Ok ?
            result.interrupts.result != gicv2::RuntimeResult::Ok ? "GPU INITIALIZED - NATIVE IRQ BLOCKED" :
                "GPU INITIALIZED - SMMU BLOCKED" : "NATIVE GPU ACTIVATION STOPPED";
    Line(framebuffer, 0, heading, color);
    Line(framebuffer, 1, "NO WINDOWS OR UEFI SERVICES", color);
    Line(framebuffer, 2, nativeGpuStage, color);
    Line(framebuffer, 3, spmi::resultName(handoff.supply), color);
    Number(framebuffer, 4, "RAIL UV", handoff.rail.final.microvolts, color);
    Number(framebuffer, 5, "SPMI STATUS", result.bus.status, color);
    Number(framebuffer, 6, "SPMI VERSION", result.bus.version, color);
    Number(framebuffer, 7, "SPMI COMMAND", result.bus.command, color);
    Line(framebuffer, 8, ResultName(handoff.clock), color);
    Number(framebuffer, 9, "POWER RESULT", static_cast<u32>(result.powerResult), color);
    Number(framebuffer, 10, "GX GDSCR", observed.gx, color);
    Number(framebuffer, 11, "CX GDSCR", observed.cx, color);
    Number(framebuffer, 12, "CORE CBCR", observed.coreClock, color);
    Number(framebuffer, 13, "AHB CBCR", observed.ahbClock, color);
    Number(framebuffer, 14, "TIMER CBCR", observed.timerClock, color);
    Number(framebuffer, 15, "GPU VERSION", result.snapshot.version, color);
    Number(framebuffer, 16, "GPU CONFIG", result.snapshot.configuration, color);
    Number(framebuffer, 17, "HW INIT", static_cast<u32>(result.hardwareResult), color);
    Number(framebuffer, 18, "HW INDEX", result.hardware.lastIndex, color);
    Number(framebuffer, 19, "HW VALUE", result.hardware.lastValue, color);
    Number(framebuffer, 20, "SCM RESULT", static_cast<u32>(result.secureResult), color);
    Number(framebuffer, 21, "SCM AVAILABLE", result.configuration.availability, color);
    Number(framebuffer, 22, "SCM MONITOR", result.configuration.returned.values[0], color);
    Number(framebuffer, 23, "SCM SERVICE", result.configuration.returned.values[1], color);
    Number(framebuffer, 24, "SCM RESUMES", result.configuration.resumes, color);
    Number(framebuffer, 25, "PMIC TYPE/SUB", (static_cast<u32>(handoff.inherited.type) << 8) |
        handoff.inherited.subtype, color);
    const spmi::RailSnapshot rails[] = {handoff.rail.initial, handoff.rail.final};
    const char *railLabels[] = {"RAIL IN CFG", "RAIL OUT CFG"};
    for (u32 index = 0; index < 2; ++index) {
        const auto &rail = rails[index];
        Number(framebuffer, 26 + index, railLabels[index], (static_cast<u32>(rail.range) << 24) |
            (static_cast<u32>(rail.voltage) << 16) | (static_cast<u32>(rail.mode) << 8) | rail.enable, color);
    }
    Number(framebuffer, 28, "RAIL IN STATUS", handoff.rail.initial.status, color);
    Number(framebuffer, 29, "RAIL OUT STATUS", handoff.rail.final.status, color);
    Number(framebuffer, 30, "CB0 SCTLR", result.contexts.banks[0].control, color);
    Number(framebuffer, 31, "CB1 SCTLR", result.contexts.banks[1].control, color);
    Number(framebuffer, 32, "CB0 FSR", result.contexts.banks[0].fault, color);
    Number(framebuffer, 33, "CB1 FSR", result.contexts.banks[1].fault, color);
    Number(framebuffer, 34, "CB0 TTBR0 LO", result.contexts.banks[0].tableBase[0], color);
    Number(framebuffer, 35, "CB1 TTBR0 LO", result.contexts.banks[1].tableBase[0], color);
    Number(framebuffer, 36, "CB0 READ MASK", result.contexts.banks[0].validFields, color);
    Number(framebuffer, 37, "CB1 READ MASK", result.contexts.banks[1].validFields, color);
    Number(framebuffer, 38, "SMMU LAST OFF", result.contexts.lastOffset, color);
    Number(framebuffer, 39, "SMMU READS", result.contexts.successfulReads, color);
    Number(framebuffer, 40, "CONTEXT RESULT", static_cast<u32>(result.contextResult), color);
    Number(framebuffer, 41, "ABORT ESR", native_mmio_esr, color);
    Number(framebuffer, 42, "ABORT FAR", native_mmio_far, color);
    Number(framebuffer, 43, "ABORT ELR", native_mmio_elr, color);
    Number(framebuffer, 44, "IRQ RESULT", static_cast<u32>(result.interrupts.result), color);
    Number(framebuffer, 45, "NATIVE TIMER IRQ", result.interrupts.runtime.timerInterrupts(), color);
    Number(framebuffer, 46, "NATIVE SGI IRQ", result.interrupts.runtime.sgiInterrupts(), color);
    Number(framebuffer, 47, "UNEXPECTED IRQ", result.interrupts.runtime.unexpectedInterrupts(), color);
    Number(framebuffer, 48, "LAST NATIVE IRQ", result.interrupts.runtime.lastInterrupt(), color);
    Number(framebuffer, 49, "SCM CONVENTION", static_cast<u32>(result.configuration.convention), color);
    Number(framebuffer, 50, "SCM FEATURE 10", result.configuration.featureVersion, color);
    Line(framebuffer, 51, "PINNED - NO SHADER/IOS - FORCE REBOOT", color);
    Flush(framebuffer, 0, framebuffer.height);
    for (;;) { asm volatile("wfi"); }
}

[[noreturn]] void RunNativeGpuPower(const ProbeFramebuffer &framebuffer, SmdPipe &pipe)
{
    NativeGpuResult result = {};
    result.handoff.supply = spmi::Result::NotReady;
    result.handoff.clock = rpm::Result::NotReady;
    result.powerResult = power::Result::NotReady;
    result.hardwareResult = HardwareResult::NotReady;
    result.secureResult = secure::Result::NotReady;
    result.contextResult = secure::Result::NotReady;
    NativeGpuContext context = {};
    const RegisterIo registers = {&context, NativeGpuRead, NativeGpuWrite, NativeGpuBarrier, Delay};
    NativeGpuProgress(framebuffer, "CHECKING COLD GRAPHICS DOMAINS");
    PowerSnapshot initial = {};
    if (readPower(registers, &initial) == ProbeResult::IoError ||
        ((initial.gx | initial.cx) & lumia950::kGdscrPowerOn) ||
        !(initial.gx & kGdscrSwCollapse) || !(initial.cx & kGdscrSwCollapse) ||
        gdscrUnderHardwareControl(initial.gx) || gdscrUnderHardwareControl(initial.cx) ||
        ((initial.coreClock | initial.ahbClock | initial.timerClock) & kBranchEnable)) {
        NativeGpuResultScreen(framebuffer, result);
    }
    NativeGpuProgress(framebuffer, "PREPARING RPM-MANAGED STARTUP PROFILE");
    const spmi::Io supplyIo = {nullptr, NativeSpmiRead, NativeSpmiWrite, NativeGpuBarrier, Delay};
    result.handoff.supply = spmi::bind(result.bus, supplyIo, true, true);
    if (result.handoff.supply != spmi::Result::Ok) { NativeGpuResultScreen(framebuffer, result); }
    result.handoff.clock = rpm::bindSession(result.session, pipeTransport(pipe), 0x41810000, true);
    if (result.handoff.clock != rpm::Result::Ok) { NativeGpuResultScreen(framebuffer, result); }
    if (pinned::prepare(result.handoff, result.bus, result.session, true) != pinned::Result::Ok) {
        nativeGpuStage = pinned::phaseName(result.handoff.phase);
        NativeGpuResultScreen(framebuffer, result);
    }
    NativeGpuProgress(framebuffer, "ENABLING OXILI DOMAINS AND CLOCKS");
    const power::PinnedDependencies dependencies = {result.handoff.rail.final.microvolts,
        pinned::kGraphicsKHz, result.handoff.rail.ready, result.handoff.clock == rpm::Result::Ok,
        true, true, result.handoff.rail.rpmManaged};
    result.powerResult = power::startPinned(result.power, registers, dependencies, 10000);
    if (result.powerResult != power::Result::Ok) { NativeGpuResultScreen(framebuffer, result); }
    NativeGpuProgress(framebuffer, "VERIFYING SUPPLY BEFORE GPU ACCESS");
    result.handoff.supply = spmi::recheckGraphicsRail(result.bus, result.handoff.rail, 10000);
    if (result.handoff.supply != spmi::Result::Ok) { NativeGpuResultScreen(framebuffer, result); }
    context.gpuAccessible = true;
    NativeGpuProgress(framebuffer, "READING REAL GPU HARDWARE");
    result.identity = readSnapshot(registers, &result.snapshot);
    if (result.identity != ProbeResult::Ready || !plausibleVersion(result.snapshot.version)) {
        NativeGpuResultScreen(framebuffer, result);
    }
    NativeGpuProgress(framebuffer, "INITIALIZING A418 COMMAND HARDWARE");
    const HardwareContract contract = {lumia950::kChipId, true, true};
    result.hardwareResult = initializeCommandHardware(result.hardware, registers, contract, 10000);
    if (result.hardwareResult != HardwareResult::Ok) { NativeGpuResultScreen(framebuffer, result); }
    NativeGpuProgress(framebuffer, "VERIFYING SUPPLY AFTER HARDWARE INIT");
    result.handoff.supply = spmi::recheckGraphicsRail(result.bus, result.handoff.rail, 10000);
    if (result.handoff.supply != spmi::Result::Ok) { NativeGpuResultScreen(framebuffer, result); }
    if (nativeGpuMode == ProbeGpuMode::GpuOnly) {
        nativeGpuStage = "GPU ONLY - SECURE CALLS SKIPPED";
        result.baselineComplete = true;
        NativeGpuResultScreen(framebuffer, result);
    }
    if (!ProbeGpuAllowsSecure(nativeGpuMode)) { NativeGpuResultScreen(framebuffer, result); }
    NativeGpuProgress(framebuffer, "STARTING THE NATIVE INTERRUPT RUNTIME");
    if (NativeGpuStartInterrupts(result.interrupts) != gicv2::RuntimeResult::Ok) {
        nativeGpuStage = gicv2::runtimeResultName(result.interrupts.result);
        NativeGpuResultScreen(framebuffer, result);
    }
    NativeGpuProgress(framebuffer, "NATIVE TIMER AND SGI IRQ VERIFIED");
    NativeGpuProgress(framebuffer, "RESTORING OEM SECURE SMMU POLICY");
    NativeScmTrace trace = {&framebuffer, 0, secure::Request::Invalid, false};
    const secure::MonitorIo monitor = {&trace, NativeScmInvoke};
    result.secureResult = secure::restoreGraphicsConfiguration(result.configuration, monitor,
        {true, true, true, nativeSmmuProfile, ProbeGpuAllowsSecure(nativeGpuMode)}, 64);
    if (result.secureResult != secure::Result::Ok) { NativeGpuResultScreen(framebuffer, result); }
    NativeGpuProgress(framebuffer, "READING NONSECURE GPU CONTEXT BANKS");
    result.contextResult = secure::inspectGraphicsContexts(result.contexts, registers, result.configuration);
    result.complete = result.contextResult == secure::Result::Ok;
    NativeGpuResultScreen(framebuffer, result);
}
