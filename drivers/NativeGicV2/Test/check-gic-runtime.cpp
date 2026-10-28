#include "../GicRuntimeCore.h"
#include <array>
#include <cstdio>
#include <deque>
#include <vector>

using namespace gicv2;

namespace {
unsigned checks = 0;
unsigned failures = 0;

void expect(bool condition, const char *name)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

struct FakeRuntime {
    std::array<u32, 1024> distributor = {};
    std::array<u32, 1024> cpu = {};
    std::deque<u32> pending;
    std::vector<u32> ended;
    u64 now = 0;
    u64 deadline = 0;
    unsigned writes = 0;
    unsigned timerArms = 0;
    bool irq = false;
    bool timer = false;
    bool rejectTimer = false;
    bool rejectCpu = false;
    bool rejectPmr = false;
    bool secure = false;
    bool repeat = false;
    u32 priorityBits = 0xff;

    FakeRuntime()
    {
        distributor[0xfe8 / 4] = 0x20;
        distributor[4 / 4] = 31 | (1u << 10);
        for (u32 offset = 0x800; offset < 0x820; offset += 4) distributor[offset / 4] = 0x04040404;
        cpu[0] = 0x600;
        cpu[0xfc / 4] = 0x0202143b;
    }

    static u32 read(void *opaque, Space space, u32 offset)
    {
        auto &fake = *static_cast<FakeRuntime *>(opaque);
        if (space == Space::Cpu) {
            if (offset == 4 && fake.rejectPmr) return 0;
            if (offset == 12) {
                if (fake.pending.empty()) return 1023;
                const u32 value = fake.pending.front();
                if (!fake.repeat) fake.pending.pop_front();
                return value;
            }
            return fake.cpu[offset / 4];
        }
        if (offset >= 0x80 && offset < 0x100 && !fake.secure) return 0;
        return fake.distributor[offset / 4];
    }

    static void write(void *opaque, Space space, u32 offset, u32 value)
    {
        auto &fake = *static_cast<FakeRuntime *>(opaque);
        ++fake.writes;
        if (space == Space::Cpu) {
            fake.cpu[offset / 4] = offset == 4 ? value & fake.priorityBits : value;
            if (offset == 16) fake.ended.push_back(value);
            return;
        }
        if (offset >= 0x80 && offset < 0x100 && !fake.secure) return;
        if (offset >= 0x800 && offset < 0x820) return;
        if (offset >= 0x100 && offset < 0x180) fake.distributor[offset / 4] |= value;
        else if (offset >= 0x180 && offset < 0x200) fake.distributor[(offset - 0x80) / 4] &= ~value;
        else fake.distributor[offset / 4] = offset >= 0x400 && offset < 0x800 ?
            value & (fake.priorityBits * 0x01010101u) : value;
    }

    static void barrier(void *) {}
    static u64 counter(void *opaque) { return static_cast<FakeRuntime *>(opaque)->now; }
    static bool arm(void *opaque, u64 value)
    {
        auto &fake = *static_cast<FakeRuntime *>(opaque);
        ++fake.timerArms;
        fake.deadline = value;
        fake.timer = !fake.rejectTimer;
        return fake.timer;
    }
    static bool stop(void *opaque) { static_cast<FakeRuntime *>(opaque)->timer = false; return true; }
    static bool setIrq(void *opaque, bool enabled)
    {
        auto &fake = *static_cast<FakeRuntime *>(opaque);
        fake.irq = enabled && !fake.rejectCpu;
        return !fake.rejectCpu;
    }
    static bool irqEnabled(void *opaque) { return static_cast<FakeRuntime *>(opaque)->irq; }
    RegisterIo registers() { return {this, read, write, barrier}; }
    RuntimeClock clock() { return {this, counter, arm, stop, setIrq, irqEnabled}; }
};

RuntimeContract contract() { return {true, true, true, true, 27, 19200000, 10000, 100000}; }

void proof(FakeRuntime &fake, Runtime &runtime)
{
    expect(runtime.begin(fake.registers(), fake.clock(), contract()) == RuntimeResult::Pending, "runtime begins but does not claim IRQ proof");
    expect(!runtime.ready() && runtime.poll() == RuntimeResult::Pending, "time and programmed registers alone do not qualify delivery");
    fake.pending.push_back(3u << 10);
    runtime.dispatch();
    expect(runtime.sgiInterrupts() == 1 && fake.ended.back() == 3u << 10, "SGI source CPU bits are preserved in EOIR");
    for (unsigned count = 0; count < 2; ++count) {
        fake.now = fake.deadline;
        fake.pending.push_back(27);
        runtime.dispatch();
        expect(runtime.timerInterrupts() == count + 1, "timer IRQ handler runs and rearms");
    }
    expect(runtime.poll() == RuntimeResult::Ok && runtime.ready(), "two timer IRQs and real SGI dispatch qualify runtime");
}

void testProof()
{
    FakeRuntime fake;
    Runtime runtime;
    proof(fake, runtime);
    expect(fake.irq && fake.timer && fake.timerArms == 3, "native IRQ and periodic timer remain enabled for secure calls");
    expect(runtime.begin(fake.registers(), fake.clock(), contract()) == RuntimeResult::NotReady, "runtime cannot be rebound");
    runtime.stop();
    expect(!fake.irq && !fake.timer && !(fake.cpu[0] & 3), "stop masks CPU IRQ, timer and GICC");
    expect(!runtime.ready() && runtime.poll() == RuntimeResult::NotReady, "stopped runtime does not authorize monitor calls");
}

void testContracts()
{
    for (unsigned field = 0; field < 4; ++field) {
        FakeRuntime fake;
        Runtime runtime;
        auto policy = contract();
        if (field == 0) policy.platformQualified = false;
        if (field == 1) policy.firmwareExited = false;
        if (field == 2) policy.primaryCpu = false;
        if (field == 3) policy.exclusiveNonsecure = false;
        expect(runtime.begin(fake.registers(), fake.clock(), policy) == RuntimeResult::Unqualified, "unqualified platform ownership is refused");
        expect(fake.writes == 0 && fake.timerArms == 0, "unqualified contract performs no hardware writes");
    }
    for (unsigned field = 0; field < 15; ++field) {
        FakeRuntime fake;
        Runtime runtime;
        auto policy = contract();
        auto clock = fake.clock();
        auto registers = fake.registers();
        if (field == 0) registers.read32 = nullptr;
        if (field == 1) registers.write32 = nullptr;
        if (field == 2) registers.barrier = nullptr;
        if (field == 3) clock.counter = nullptr;
        if (field == 4) clock.armTimer = nullptr;
        if (field == 5) clock.stopTimer = nullptr;
        if (field == 6) clock.setCpuIrq = nullptr;
        if (field == 7) clock.cpuIrqEnabled = nullptr;
        if (field == 8) policy.frequency = 0;
        if (field == 9) policy.frequency = 1000000001;
        if (field == 10) policy.periodMicroseconds = 999;
        if (field == 11) policy.proofMicroseconds = 29999;
        if (field == 12) policy.proofMicroseconds = 200001;
        if (field == 13) policy.timerInterrupt = 15;
        if (field == 14) policy.timerInterrupt = 32;
        expect(runtime.begin(registers, clock, policy) == RuntimeResult::Invalid, "missing callbacks or invalid timing are refused");
        expect(fake.writes == 0 && fake.timerArms == 0, "invalid configuration performs no hardware writes");
    }
}

void testFailures()
{
    for (unsigned mode = 0; mode < 6; ++mode) {
        FakeRuntime fake;
        Runtime runtime;
        if (mode == 0) fake.secure = true;
        if (mode == 1) fake.rejectPmr = true;
        if (mode == 2) fake.rejectTimer = true;
        if (mode == 3) fake.rejectCpu = true;
        if (mode == 4) fake.distributor[0xfe8 / 4] = 0x30;
        if (mode == 5) {
            for (u32 offset = 0x800; offset < 0x820; offset += 4) fake.distributor[offset / 4] = 0;
        }
        const auto result = runtime.begin(fake.registers(), fake.clock(), contract());
        expect(result == RuntimeResult::ControllerRejected || result == RuntimeResult::TimerRejected, "hardware refusal cannot qualify IRQ runtime");
        expect(!runtime.ready() && !fake.timer && !(fake.cpu[0] & 3), "failure leaves local interrupt delivery disabled");
    }
    FakeRuntime missing;
    Runtime noInterrupt;
    expect(noInterrupt.begin(missing.registers(), missing.clock(), contract()) == RuntimeResult::Pending, "missing-interrupt test begins");
    missing.now = 1920001;
    expect(noInterrupt.poll() == RuntimeResult::IrqMissing && !noInterrupt.ready(), "bounded proof cannot pass with no ISR delivery");
    FakeRuntime stale;
    Runtime runtime;
    proof(stale, runtime);
    stale.now += 1920001;
    expect(!runtime.ready() && runtime.poll() == RuntimeResult::IrqMissing, "stale heartbeat cannot authorize a secure call");
    FakeRuntime masked;
    Runtime maskedRuntime;
    proof(masked, maskedRuntime);
    masked.irq = false;
    expect(!maskedRuntime.ready() && maskedRuntime.poll() == RuntimeResult::IrqMissing, "masked CPU IRQ invalidates qualification");
}

void testInterruptErrors()
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        FakeRuntime fake;
        Runtime runtime;
        proof(fake, runtime);
        if (mode == 0) fake.pending.push_back(40);
        if (mode == 1) { fake.pending.push_back(27); fake.rejectTimer = true; }
        if (mode == 2) { fake.pending.push_back(0); fake.repeat = true; }
        runtime.dispatch();
        const auto expected = mode == 0 ? RuntimeResult::UnexpectedInterrupt :
            mode == 1 ? RuntimeResult::TimerRejected : RuntimeResult::InterruptStorm;
        expect(runtime.result() == expected && runtime.state() == RuntimeState::Failed, "unexpected IRQ, rearm failure or storm stops qualification");
        expect(!runtime.ready() && !fake.timer && !(fake.cpu[0] & 3), "ISR failure disables local delivery and invalidates secure calls");
    }
    FakeRuntime spurious;
    Runtime runtime;
    proof(spurious, runtime);
    const auto ended = spurious.ended.size();
    runtime.dispatch();
    expect(spurious.ended.size() == ended && runtime.ready(), "spurious acknowledge is not ended or counted as proof");
}

void testLumiaTimer()
{
    FakeRuntime fake;
    Runtime runtime;
    auto policy = contract();
    policy.timerInterrupt = 20;
    expect(runtime.begin(fake.registers(), fake.clock(), policy) == RuntimeResult::Pending, "Lumia GTDT PPI 20 is configurable");
    expect((fake.distributor[0x100 / 4] & (1u | (1u << 20))) == (1u | (1u << 20)), "SGI 0 and Lumia virtual timer PPI 20 are enabled");
    expect(!(fake.distributor[0x100 / 4] & (1u << 27)), "QEMU timer PPI 27 is not enabled on Lumia profile");
    fake.pending.push_back(0);
    runtime.dispatch();
    for (unsigned count = 0; count < 2; ++count) {
        fake.now = fake.deadline;
        fake.pending.push_back(20);
        runtime.dispatch();
    }
    expect(runtime.poll() == RuntimeResult::Ok && runtime.ready(), "Lumia timer delivery and rearm qualify independently of QEMU IRQ numbering");
    fake.pending.push_back(27);
    runtime.dispatch();
    expect(runtime.result() == RuntimeResult::UnexpectedInterrupt, "wrong-platform timer IRQ is not accepted as a heartbeat");
}

void testLiveController()
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        FakeRuntime fake;
        Runtime runtime;
        proof(fake, runtime);
        if (mode == 0) fake.distributor[0] = 0;
        if (mode == 1) fake.cpu[0] = 0;
        if (mode == 2) fake.cpu[0] |= 1u << 9;
        if (mode == 3) fake.cpu[1] = 0;
        expect(!runtime.ready() && runtime.poll() == RuntimeResult::IrqMissing,
            "lost distributor, CPU interface, EOI mode or priority access revokes qualification");
        expect(!fake.irq && !fake.timer, "lost controller state stops native delivery");
    }
    FakeRuntime wrapped;
    Runtime runtime;
    wrapped.now = ~0ull - 100000;
    proof(wrapped, runtime);
    expect(runtime.ready(), "counter wrap does not invalidate a fresh IRQ heartbeat");
    wrapped.now += 1920001;
    expect(runtime.poll() == RuntimeResult::IrqMissing, "stale heartbeat still expires across counter wrap");
}

void testPriorityPrecision()
{
    for (u32 bits : {0xf0u, 0xf8u, 0xfcu, 0xfeu, 0xffu}) {
        FakeRuntime fake;
        fake.priorityBits = bits;
        Runtime runtime;
        proof(fake, runtime);
        expect(runtime.ready() && runtime.controller().priorityMaskBits() == bits,
            "IRQ delivery proof remains required for every implemented priority precision");
        fake.cpu[1] = 0;
        expect(runtime.poll() == RuntimeResult::IrqMissing,
            "losing PMR access revokes qualification even with four-bit priorities");
    }
}

void testCommonIdentity()
{
    FakeRuntime fake;
    fake.distributor[0xfe8 / 4] = 0;
    Runtime runtime;
    proof(fake, runtime);
    expect(runtime.controller().identity() == Identity::CpuInterface && runtime.ready(),
        "missing distributor peripheral ID still requires actual IRQ proof after common CPU identification");
    FakeRuntime unknown;
    unknown.distributor[0xfe8 / 4] = 0;
    unknown.cpu[0xfc / 4] = 0;
    Runtime rejected;
    expect(rejected.begin(unknown.registers(), unknown.clock(), contract()) == RuntimeResult::ControllerRejected &&
        !unknown.writes && !unknown.timerArms && !unknown.irq,
        "missing common identity cannot authorize any GIC write, timer arm or SCM entry");
}
}

int main()
{
    testProof();
    testContracts();
    testFailures();
    testInterruptErrors();
    testLumiaTimer();
    testLiveController();
    testPriorityPrecision();
    testCommonIdentity();
    std::printf("GicRuntimeCore: %u checks, %u failures (simulated IRQ delivery, not hardware)\n", checks, failures);
    return failures ? 1 : 0;
}
