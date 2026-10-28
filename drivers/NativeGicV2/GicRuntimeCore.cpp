#include "GicRuntimeCore.h"

namespace gicv2 {

RuntimeResult Runtime::fail(RuntimeResult result)
{
    if (stage == RuntimeState::Failed) return outcome;
    outcome = result;
    stage = RuntimeState::Failed;
    if (timer.stopTimer) timer.stopTimer(timer.context);
    interruptController.disableLocal();
    if (timer.setCpuIrq) timer.setCpuIrq(timer.context, false);
    return result;
}

RuntimeResult Runtime::begin(const RegisterIo &registers, const RuntimeClock &clock,
    const RuntimeContract &contract)
{
    if (stage != RuntimeState::Empty) return RuntimeResult::NotReady;
    if (!contract.platformQualified || !contract.firmwareExited || !contract.primaryCpu ||
        !contract.exclusiveNonsecure) return fail(RuntimeResult::Unqualified);
    if (!registers.read32 || !registers.write32 || !registers.barrier ||
        !clock.counter || !clock.armTimer || !clock.stopTimer || !clock.setCpuIrq ||
        !clock.cpuIrqEnabled || contract.frequency < 1000000 || contract.frequency > 1000000000 ||
        contract.timerInterrupt < 16 || contract.timerInterrupt > 31 ||
        contract.periodMicroseconds < 1000 || contract.periodMicroseconds > 10000 ||
        contract.proofMicroseconds < contract.periodMicroseconds * 3 ||
        contract.proofMicroseconds > 200000) return fail(RuntimeResult::Invalid);
    stage = RuntimeState::Preparing;
    timer = clock;
    timerInterrupt = contract.timerInterrupt;
    period = (contract.frequency * contract.periodMicroseconds + 999999) / 1000000;
    proofBudget = (contract.frequency * contract.proofMicroseconds + 999999) / 1000000;
    if (!timer.setCpuIrq(timer.context, false) || timer.cpuIrqEnabled(timer.context) ||
        !timer.stopTimer(timer.context)) return fail(RuntimeResult::TimerRejected);
    if (!interruptController.configure(registers, 0x1000, 0x1000) ||
        !interruptController.prepareDistributor(true) || !interruptController.prepareLocal(timerInterrupt) ||
        !interruptController.enableLocal()) return fail(RuntimeResult::ControllerRejected);
    started = timer.counter(timer.context);
    lastTimer = started;
    stage = RuntimeState::Proving;
    outcome = RuntimeResult::Pending;
    if (!timer.armTimer(timer.context, started + period) ||
        !interruptController.sendSgi(interruptController.cpuTarget()) ||
        !timer.setCpuIrq(timer.context, true) || !timer.cpuIrqEnabled(timer.context)) {
        return fail(RuntimeResult::TimerRejected);
    }
    return RuntimeResult::Pending;
}

bool Runtime::ready() const
{
    const u64 observed = lastTimer;
    return stage == RuntimeState::Ready && outcome == RuntimeResult::Ok &&
        timers >= 2 && sgis >= 1 && !unexpected && timer.cpuIrqEnabled &&
        timer.cpuIrqEnabled(timer.context) &&
        timer.counter(timer.context) - observed <= proofBudget &&
        (interruptController.read(Space::Distributor, 0) & 1) &&
        (interruptController.read(Space::Cpu, 0) & 0x601) == 1 &&
        interruptController.priorityMaskEnabled() &&
        stage == RuntimeState::Ready && outcome == RuntimeResult::Ok;
}

RuntimeResult Runtime::poll()
{
    if (stage == RuntimeState::Failed) return outcome;
    if (stage == RuntimeState::Ready) return ready() ? RuntimeResult::Ok : fail(RuntimeResult::IrqMissing);
    if (stage != RuntimeState::Proving) return RuntimeResult::NotReady;
    if (timer.counter(timer.context) - started > proofBudget) return fail(RuntimeResult::IrqMissing);
    if (timers < 2 || sgis < 1) return RuntimeResult::Pending;
    if (!timer.cpuIrqEnabled(timer.context) ||
        !(interruptController.read(Space::Distributor, 0) & 1) ||
        (interruptController.read(Space::Cpu, 0) & 0x601) != 1 ||
        !interruptController.priorityMaskEnabled()) {
        return fail(RuntimeResult::ControllerRejected);
    }
    outcome = RuntimeResult::Ok;
    stage = RuntimeState::Ready;
    return RuntimeResult::Ok;
}

void Runtime::dispatch()
{
    if (stage != RuntimeState::Proving && stage != RuntimeState::Ready) {
        fail(RuntimeResult::UnexpectedInterrupt);
        return;
    }
    for (u32 drained = 0; drained < 64; ++drained) {
        const u32 raw = interruptController.acknowledge();
        const u32 identifier = interruptId(raw);
        if (identifier >= 1020) return;
        last = identifier;
        bool handled = true;
        if (identifier == timerInterrupt) {
            const u64 now = timer.counter(timer.context);
            handled = timer.stopTimer(timer.context) && timer.armTimer(timer.context, now + period);
            if (handled) {
                lastTimer = now;
                timers = timers + 1;
            }
        } else if (identifier == 0) {
            sgis = sgis + 1;
        } else {
            unexpected = unexpected + 1;
            interruptController.mask(identifier);
            handled = false;
        }
        if (!interruptController.end(raw)) {
            fail(RuntimeResult::ControllerRejected);
            return;
        }
        if (!handled) {
            fail(identifier == timerInterrupt ? RuntimeResult::TimerRejected : RuntimeResult::UnexpectedInterrupt);
            return;
        }
    }
    fail(RuntimeResult::InterruptStorm);
}

void Runtime::stop()
{
    if (timer.setCpuIrq) timer.setCpuIrq(timer.context, false);
    if (timer.stopTimer) timer.stopTimer(timer.context);
    interruptController.disableLocal();
    if (stage != RuntimeState::Failed && stage != RuntimeState::Empty) stage = RuntimeState::Stopped;
}

const char *runtimeResultName(RuntimeResult result)
{
    switch (result) {
    case RuntimeResult::Ok: return "NATIVE IRQ VERIFIED";
    case RuntimeResult::Pending: return "WAITING FOR REAL IRQ";
    case RuntimeResult::Invalid: return "INVALID IRQ RUNTIME";
    case RuntimeResult::Unqualified: return "UNQUALIFIED IRQ OWNERSHIP";
    case RuntimeResult::NotReady: return "IRQ RUNTIME NOT READY";
    case RuntimeResult::ControllerRejected: return "GIC CONFIGURATION REJECTED";
    case RuntimeResult::TimerRejected: return "TIMER SETUP REJECTED";
    case RuntimeResult::IrqMissing: return "NATIVE IRQ DELIVERY MISSING";
    case RuntimeResult::UnexpectedInterrupt: return "UNEXPECTED NATIVE INTERRUPT";
    case RuntimeResult::InterruptStorm: return "NATIVE INTERRUPT STORM";
    }
    return "UNKNOWN IRQ RESULT";
}

}
