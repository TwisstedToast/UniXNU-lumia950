#include "../../drivers/NativeGicV2/GicRuntimeCore.h"

using namespace gicv2;
using usize = __SIZE_TYPE__;

extern "C" bool NativeGuardedRead(u64, u32 *);
extern "C" bool NativeGuardedWrite(u64, u32);
extern "C" bool NativeGuardedScmCall(u64 *);
extern "C" u64 NativeInstallVectors();
extern "C" volatile u64 native_mmio_esr, native_mmio_far;
extern "C" {
volatile u32 native_monitor_stub_calls;
volatile u32 native_el2_smc_traps;
}

extern "C" void *memset(void *destination, int value, usize bytes)
{
    auto *output = static_cast<volatile u8 *>(destination);
    for (usize index = 0; index < bytes; ++index) output[index] = static_cast<u8>(value);
    return destination;
}

extern "C" void *memcpy(void *destination, const void *source, usize bytes)
{
    auto *output = static_cast<u8 *>(destination);
    const auto *input = static_cast<const u8 *>(source);
    for (usize index = 0; index < bytes; ++index) output[index] = input[index];
    return destination;
}

namespace {
Runtime *activeRuntime;
volatile u32 nestedGuards;
constexpr u64 kInvalid = 0xffff000000000000ull;

void print(const char *message)
{
    auto *uart = reinterpret_cast<volatile u32 *>(0x09000000ull);
    while (*message) {
        while (uart[0x18 / 4] & 0x20) asm volatile("yield");
        uart[0] = static_cast<u8>(*message++);
    }
}

[[noreturn]] void finish(u64 status)
{
    asm volatile("msr daifset, #15" ::: "memory");
    const u64 arguments[2] = {0x20026, status};
    register u64 operation asm("x0") = 0x20;
    register const u64 *parameters asm("x1") = arguments;
    asm volatile("hlt #0xf000" : "+r"(operation) : "r"(parameters) : "memory");
    for (;;) asm volatile("wfe");
}

[[noreturn]] void fail(const char *message)
{
    print("FAIL ");
    print(message);
    print("\n");
    finish(1);
}

u32 read(void *, Space space, u32 offset)
{
    u32 value = 0xffffffff;
    const u64 base = space == Space::Distributor ? 0x08000000ull : 0x08010000ull;
    if (!NativeGuardedRead(base + offset, &value)) fail("GIC read guard");
    return value;
}

void write(void *, Space space, u32 offset, u32 value)
{
    const u64 base = space == Space::Distributor ? 0x08000000ull : 0x08010000ull;
    if (!NativeGuardedWrite(base + offset, value)) fail("GIC write guard");
}

void barrier(void *) { asm volatile("dsb sy; isb" ::: "memory"); }
u64 counter(void *)
{
    u64 value;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

bool arm(void *, u64 deadline)
{
    asm volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb" ::
        "r"(deadline), "r"(static_cast<u64>(1)) : "memory");
    return true;
}

bool stop(void *) { asm volatile("msr cntv_ctl_el0, xzr; isb" ::: "memory"); return true; }
bool enabled(void *)
{
    u64 mask;
    asm volatile("mrs %0, daif" : "=r"(mask));
    return !(mask & 0x80);
}

bool setEnabled(void *, bool enable)
{
    if (enable) asm volatile("msr daifclr, #2; isb" ::: "memory");
    else asm volatile("msr daifset, #2; isb" ::: "memory");
    return enabled(nullptr) == enable;
}
}

extern "C" [[noreturn]] void NativeFatalFault() { fail("native exception vector"); }
extern "C" [[noreturn]] void gic_test_exception() { fail("entry exception vector"); }

extern "C" bool NativeIrqDispatch()
{
    if (!activeRuntime) fail("unowned native IRQ");
    u32 unchanged = 0x4188992;
    if (NativeGuardedRead(kInvalid, &unchanged) || unchanged != 0x4188992 ||
        (native_mmio_esr >> 26) != 0x25 || native_mmio_far != kInvalid) {
        fail("nested data-abort guard inside IRQ");
    }
    nestedGuards = nestedGuards + 1;
    activeRuntime->dispatch();
    return activeRuntime->state() != RuntimeState::Failed;
}

extern "C" void gic_test_irq() { fail("old test vectors remained installed"); }

extern "C" [[noreturn]] void gic_test_main()
{
    u64 frequency, currentEl;
    asm volatile("mrs %0, cntfrq_el0; mrs %1, CurrentEL" : "=r"(frequency), "=r"(currentEl));
    if (currentEl != 4) fail("native runtime requires EL1");
    NativeInstallVectors();
    Runtime runtime;
    activeRuntime = &runtime;
    const RegisterIo registers = {nullptr, read, write, barrier};
    const RuntimeClock clock = {nullptr, counter, arm, stop, setEnabled, enabled};
    const RuntimeContract policy = {true, true, true, true, 27, frequency, 10000, 100000};
    RuntimeResult result = runtime.begin(registers, clock, policy);
    if (result == RuntimeResult::ControllerRejected) fail("GICC enable");
    for (u32 polls = 0; result == RuntimeResult::Pending && polls < 10000000; ++polls) {
        result = runtime.poll();
        asm volatile("yield" ::: "memory");
    }
    if (result != RuntimeResult::Ok || !runtime.ready() || runtime.timerInterrupts() < 2 ||
        runtime.sgiInterrupts() != 1 || nestedGuards < 3) fail("native asynchronous proof");
    volatile u32 storage = 0x418;
    u32 readback = 0;
    if (!NativeGuardedRead(reinterpret_cast<u64>(&storage), &readback) || readback != 0x418 ||
        !NativeGuardedWrite(reinterpret_cast<u64>(&storage), 0x8992) || storage != 0x8992) {
        fail("MMIO guard with IRQ runtime active");
    }
    u64 values[8] = {0x02000601, 1, 0x02000602, 0, 0, 0, 0, 0};
    const bool returned = NativeGuardedScmCall(values);
    const bool security = (read(nullptr, Space::Distributor, 4) & (1u << 10)) != 0;
    if (returned) {
        if (values[0] != ~0ull && values[0] != 0xffffffffu) fail("unsupported monitor status");
        if (native_monitor_stub_calls != (security ? 1u : 0u) ||
            native_el2_smc_traps != (security ? 1u : 0u)) {
            fail("synthetic EL2/EL3 forwarding return");
        }
        print(security ? "SYNTHETIC MONITOR UNSUPPORTED - EL2 FORWARDING TESTED - NOT HANDSET SCM\n" :
            "NO EL3 - NATIVE SCM GUARD RETURNED UNSUPPORTED\n");
    } else {
        if (security || (native_mmio_esr >> 26) != 0 || native_monitor_stub_calls || native_el2_smc_traps) {
            fail("SMC undefined guard without EL3");
        }
        print("NO EL3 - NATIVE SCM GUARD RECOVERED UNDEFINED TRAP\n");
    }
    const u32 before = runtime.timerInterrupts();
    const u64 started = counter(nullptr);
    while (runtime.timerInterrupts() <= before) {
        if (counter(nullptr) - started > frequency / 10) fail("timer did not survive secure-call guard");
        asm volatile("yield" ::: "memory");
    }
    if (!runtime.ready() || runtime.unexpectedInterrupts() || nestedGuards < 4) {
        fail("guard round trip corrupted native IRQ runtime");
    }
    if (!runtime.controller().configureSpi(40, true) || !runtime.controller().enable(40)) {
        fail("unhandled interrupt test setup");
    }
    write(nullptr, Space::Distributor, 0x204, 1u << 8);
    const u64 unexpectedStarted = counter(nullptr);
    while (runtime.state() != RuntimeState::Failed) {
        if (counter(nullptr) - unexpectedStarted > frequency / 10) fail("unhandled interrupt was not delivered");
        asm volatile("yield" ::: "memory");
    }
    if (enabled(nullptr) || runtime.result() != RuntimeResult::UnexpectedInterrupt ||
        runtime.unexpectedInterrupts() != 1) fail("failed ISR did not return with IRQ masked");
    runtime.stop();
    if (enabled(nullptr) || runtime.ready() || (read(nullptr, Space::Cpu, 0) & 3)) fail("runtime shutdown");
    print("PASS NATIVE IRQ RUNTIME TIMER_REARM SGI NESTED_ABORT SCM_GUARD MMIO_GUARD ISR_ERROR_MASK\n");
    finish(0);
}
