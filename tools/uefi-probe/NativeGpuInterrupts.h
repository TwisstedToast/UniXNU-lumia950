const MemoryRegion nativeRuntimeRegions[] = {
    {0xf9000000, 0xf9000000, 0x1000},
    {0xf9002000, 0xf9002000, 0x1000},
    {0xf900d000, 0xf900d000, 0x1000}
};

struct NativeInterruptRegister { gicv2::Space space; u32 offset; const char *label; };
const NativeInterruptRegister nativeInterruptRegisters[] = {
    {gicv2::Space::Distributor, 0, "NOW DIST CTLR"},
    {gicv2::Space::Cpu, 0, "NOW CPU CTLR"},
    {gicv2::Space::Cpu, 4, "NOW CPU PMR"},
    {gicv2::Space::Distributor, 0xfe8, "DIST PIDR2"},
    {gicv2::Space::Cpu, 0xfc, "CPU IIDR"},
    {gicv2::Space::Distributor, 8, "DIST IIDR"},
    {gicv2::Space::Distributor, 4, "DIST TYPER"},
    {gicv2::Space::Distributor, 0x100, "LOCAL ENABLE"},
    {gicv2::Space::Distributor, 0x200, "LOCAL PENDING"},
    {gicv2::Space::Distributor, 0x80, "LOCAL GROUP"},
    {gicv2::Space::Distributor, 0x400, "SGI PRIORITIES"},
    {gicv2::Space::Distributor, 0x414, "TIMER PRIORITY"},
    {gicv2::Space::Distributor, 0x800, "CPU TARGET"}
};
constexpr u32 nativeInterruptRegisterCount = sizeof(nativeInterruptRegisters) / sizeof(nativeInterruptRegisters[0]);

struct NativeGpuInterrupts {
    gicv2::Runtime runtime;
    volatile gicv2::RuntimeResult result = gicv2::RuntimeResult::NotReady;
    u32 inheritedDistributor = 0;
    u32 inheritedCpu = 0;
    u32 inheritedPriorityMask = 0;
    volatile bool mmioFault = false;
    u32 snapshot[nativeInterruptRegisterCount] = {};
    u32 monitorEntries = 0;
};

NativeGpuInterrupts *nativeInterruptContext;

bool NativeGpuInterruptMappings(EFI_SYSTEM_TABLE *systemTable, const VOID *map,
    UINTN mapBytes, UINTN stride)
{
    const UINT8 *timer = AcpiTable(0x54445447);
    if (!timer || !isLumia950Gtdt(timer, Read32(timer + 4))) {
        Print("NATIVE_IRQ_GTDT=UNSUPPORTED\r\n");
        return false;
    }
    PrintHex("NATIVE_IRQ_VIRTUAL_TIMER_GSI", Read32(timer + 64));
    PrintHex("NATIVE_IRQ_VIRTUAL_TIMER_FLAGS", Read32(timer + 68));
    const ProbeRpmGcd gcd = ProbeFindMmioGcd(systemTable, nativeRuntimeRegions);
    PrintHex("NATIVE_IRQ_GCD_STATUS", gcd.status);
    if (!gcd.qualified || !ProbeMmioRangesReserved(map, mapBytes, stride, nativeRuntimeRegions, 3, true)) {
        return false;
    }
    for (const MemoryRegion &region : nativeRuntimeRegions) {
        for (u32 writing = 0; writing < 2; ++writing) {
            u64 par = 0;
            if (!Translate(nullptr, region.address, writing != 0, &par) || (par & 1) ||
                (par & 0x000ffffffffff000ull) != region.physical ||
                ((par >> 56) != 0 && (par >> 56) != 4)) {
                PrintHex("NATIVE_IRQ_BAD_MAPPING", region.address);
                PrintHex("NATIVE_IRQ_BAD_PAR", par);
                return false;
            }
        }
    }
    Print("NATIVE_IRQ_MAPPINGS=DEVICE_IDENTITY\r\n");
    return true;
}

u32 NativeIrqRead(void *opaque, gicv2::Space space, u32 offset)
{
    auto &context = *static_cast<NativeGpuInterrupts *>(opaque);
    u32 value = 0xffffffff;
    const u64 base = space == gicv2::Space::Distributor ? 0xf9000000ull : 0xf9002000ull;
    if (offset >= 0x1000 || (offset & 3) || !NativeRead(nullptr, base + offset, &value)) {
        context.mmioFault = true;
        return 0xffffffff;
    }
    return value;
}

void NativeIrqWrite(void *opaque, gicv2::Space space, u32 offset, u32 value)
{
    auto &context = *static_cast<NativeGpuInterrupts *>(opaque);
    if (context.mmioFault) return;
    const u64 base = space == gicv2::Space::Distributor ? 0xf9000000ull : 0xf9002000ull;
    if (offset >= 0x1000 || (offset & 3) || !NativeWrite(nullptr, base + offset, value)) {
        context.mmioFault = true;
    }
}

void NativeGpuCaptureInterrupts(NativeGpuInterrupts &context)
{
    for (u32 index = 0; index < nativeInterruptRegisterCount; ++index) {
        const auto &item = nativeInterruptRegisters[index];
        context.snapshot[index] = NativeIrqRead(&context, item.space, item.offset);
    }
}

void NativeIrqBarrier(void *) { asm volatile("dsb sy\nisb" ::: "memory"); }
gicv2::u64 NativeIrqCounter(void *) { return Counter(); }

bool NativeIrqArmTimer(void *, gicv2::u64 deadline)
{
    asm volatile("msr cntv_cval_el0, %0\nmsr cntv_ctl_el0, %1\nisb" ::
        "r"(deadline), "r"(static_cast<u64>(1)) : "memory");
    u64 control = 0;
    asm volatile("mrs %0, cntv_ctl_el0" : "=r"(control));
    return (control & 3) == 1;
}

bool NativeIrqStopTimer(void *)
{
    asm volatile("msr cntv_ctl_el0, xzr\nisb" ::: "memory");
    u64 control = 0;
    asm volatile("mrs %0, cntv_ctl_el0" : "=r"(control));
    return (control & 3) == 0;
}

bool NativeIrqEnabled(void *)
{
    u64 mask = 0;
    asm volatile("mrs %0, daif" : "=r"(mask));
    return (mask & 0x80) == 0;
}

bool NativeIrqSetEnabled(void *, bool enabled)
{
    if (enabled) { asm volatile("msr daifclr, #2\nisb" ::: "memory"); }
    else { asm volatile("msr daifset, #2\nisb" ::: "memory"); }
    return NativeIrqEnabled(nullptr) == enabled;
}

bool NativeGpuInterruptsReady()
{
    if (!nativeInterruptContext) return false;
    auto &context = *nativeInterruptContext;
    if (context.mmioFault) {
        context.result = gicv2::RuntimeResult::ControllerRejected;
        context.runtime.stop();
        return false;
    }
    if (context.result != gicv2::RuntimeResult::Ok) return false;
    context.result = context.runtime.poll();
    return context.result == gicv2::RuntimeResult::Ok && !context.mmioFault && context.runtime.ready();
}

gicv2::RuntimeResult NativeGpuStartInterrupts(NativeGpuInterrupts &context)
{
    nativeInterruptContext = &context;
    context.inheritedDistributor = NativeIrqRead(&context, gicv2::Space::Distributor, 0);
    context.inheritedCpu = NativeIrqRead(&context, gicv2::Space::Cpu, 0);
    context.inheritedPriorityMask = NativeIrqRead(&context, gicv2::Space::Cpu, 4);
    if (context.mmioFault) return context.result = gicv2::RuntimeResult::ControllerRejected;
    const gicv2::RegisterIo registers = {&context, NativeIrqRead, NativeIrqWrite, NativeIrqBarrier};
    const gicv2::RuntimeClock clock = {nullptr, NativeIrqCounter, NativeIrqArmTimer,
        NativeIrqStopTimer, NativeIrqSetEnabled, NativeIrqEnabled};
    const gicv2::RuntimeContract contract = {true, true, true, true, kLumia950VirtualTimerInterrupt,
        timerFrequency, 10000, 100000};
    context.result = context.runtime.begin(registers, clock, contract);
    u32 polls = 10000000;
    while (context.result == gicv2::RuntimeResult::Pending && polls--) {
        context.result = context.runtime.poll();
        asm volatile("yield" ::: "memory");
    }
    if (context.result == gicv2::RuntimeResult::Pending) context.result = gicv2::RuntimeResult::IrqMissing;
    if (context.mmioFault) context.result = gicv2::RuntimeResult::ControllerRejected;
    if (context.result != gicv2::RuntimeResult::Ok) context.runtime.stop();
    return context.result;
}
