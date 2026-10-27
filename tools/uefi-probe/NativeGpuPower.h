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

struct NativeGpuContext { bool gpuAccessible; };

bool NativeGpuAddress(NativeGpuContext &context, RegisterSpace space, u32 offset, u64 &address)
{
    if ((offset & 3) || offset >= 0x10000) { return false; }
    if (space == RegisterSpace::Mmss) { address = lumia950::kMmssPhys + offset; return true; }
    if (!context.gpuAccessible) { return false; }
    if (space == RegisterSpace::Gpu) { address = lumia950::kGpuPhys + offset; return true; }
    if (space == RegisterSpace::Smmu) { address = lumia950::kGpuSmmuPhys + offset; return true; }
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
    return opaque && NativeGpuAddress(*static_cast<NativeGpuContext *>(opaque), space, offset, address) &&
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
    spmi::Bus bus;
    pinned::Handoff handoff;
    rpm::Session session;
    power::Controller power;
    power::Result powerResult;
    GpuSnapshot snapshot;
    ProbeResult identity;
    CommandHardware hardware;
    HardwareResult hardwareResult;
    smmu::Routes routes;
    smmu::RouteResult routeResult;
    bool complete;
};

void NativeGpuProgress(const ProbeFramebuffer &framebuffer, const char *stage)
{
    nativeGpuStage = stage;
    const u32 color = ProbeFramebufferColor(framebuffer.pixel_format, 60, 170, 255);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, 160, 0);
    Line(framebuffer, 0, "NATIVE GPU - POST EBS", color);
    Line(framebuffer, 2, stage, color);
    Flush(framebuffer, 0, 160);
}

[[noreturn]] void NativeGpuResultScreen(const ProbeFramebuffer &framebuffer, const NativeGpuResult &result)
{
    const auto &handoff = result.handoff;
    const u32 color = ProbeFramebufferColor(framebuffer.pixel_format,
        result.complete ? 40 : 255, result.complete ? 255 : 100, 60);
    NativeGpuContext context = {};
    PowerSnapshot observed = {};
    const RegisterIo registers = {&context, NativeGpuRead, NativeGpuWrite, NativeGpuBarrier, Delay};
    readPower(registers, &observed);
    ProbeFillRectangle(&framebuffer, 0, 0, framebuffer.width, framebuffer.height, 0);
    Line(framebuffer, 0, result.complete ? "NATIVE GPU HARDWARE INITIALIZED" : "NATIVE GPU ACTIVATION STOPPED", color);
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
    Number(framebuffer, 20, "SMMU ROUTES", static_cast<u32>(result.routeResult), color);
    for (u32 index = 0; index < 3; ++index) {
        const char *labels[] = {"SMMU IDR0", "SMMU IDR1", "SMMU IDR2"};
        Number(framebuffer, 21 + index, labels[index], result.routes.id[index], color);
    }
    Number(framebuffer, 24, "SMMU CR0", result.routes.control, color);
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
    const char *streamLabels[] = {"SMMU SMR0", "SMMU SMR1", "SMMU SMR2", "SMMU SMR3"};
    const char *mappingLabels[] = {"SMMU S2CR0", "SMMU S2CR1", "SMMU S2CR2", "SMMU S2CR3"};
    for (u32 index = 0; index < 4; ++index) {
        Number(framebuffer, 30 + 2 * index, streamLabels[index], result.routes.match[index], color);
        Number(framebuffer, 31 + 2 * index, mappingLabels[index], result.routes.mapping[index], color);
    }
    Number(framebuffer, 38, "SMMU CBAR0", result.routes.attributes[0], color);
    Number(framebuffer, 39, "SMMU CBAR1", result.routes.attributes[1], color);
    Number(framebuffer, 40, "SMMU STREAMS", result.routes.count, color);
    Number(framebuffer, 41, "ABORT ESR", native_mmio_esr, color);
    Number(framebuffer, 42, "ABORT FAR", native_mmio_far, color);
    Number(framebuffer, 43, "ABORT ELR", native_mmio_elr, color);
    Number(framebuffer, 44, "QUALIFIED READS", handoff.rail.stableSamples, color);
    Number(framebuffer, 45, "CLK REQ KHZ", pinned::kGraphicsKHz, color);
    Number(framebuffer, 46, "RAIL READS", handoff.rail.samples, color);
    Number(framebuffer, 47, "RAIL MIN UV", handoff.rail.minimumMicrovolts, color);
    Number(framebuffer, 48, "RAIL MAX UV", handoff.rail.maximumMicrovolts, color);
    Line(framebuffer, 49, "NO SHADER FRAME OR IOS CLAIMED", color);
    Line(framebuffer, 50, "RESOURCES PINNED - FORCE REBOOT", color);
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
    result.routeResult = smmu::RouteResult::NotReady;
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
    NativeGpuProgress(framebuffer, "INSPECTING INHERITED SMMU ROUTES");
    result.routeResult = smmu::inspectRoutes(result.routes, registers, {true, true, true});
    result.complete = true;
    NativeGpuResultScreen(framebuffer, result);
}
