#ifndef NATIVE_GIC_V2_RUNTIME_CORE_H
#define NATIVE_GIC_V2_RUNTIME_CORE_H

#include "GicV2Core.h"

namespace gicv2 {

using u64 = unsigned long long;

struct RuntimeClock {
    void *context;
    u64 (*counter)(void *);
    bool (*armTimer)(void *, u64);
    bool (*stopTimer)(void *);
    bool (*setCpuIrq)(void *, bool);
    bool (*cpuIrqEnabled)(void *);
};

struct RuntimeContract {
    bool platformQualified;
    bool firmwareExited;
    bool primaryCpu;
    bool exclusiveNonsecure;
    u32 timerInterrupt;
    u64 frequency;
    u32 periodMicroseconds;
    u32 proofMicroseconds;
};

enum class RuntimeState : u32 { Empty, Preparing, Proving, Ready, Failed, Stopped };
enum class RuntimeResult : u32 {
    Ok, Pending, Invalid, Unqualified, NotReady, ControllerRejected,
    TimerRejected, IrqMissing, UnexpectedInterrupt, InterruptStorm
};

class Runtime {
public:
    RuntimeResult begin(const RegisterIo &registers, const RuntimeClock &clock,
        const RuntimeContract &contract);
    RuntimeResult poll();
    void dispatch();
    bool ready() const;
    void stop();
    RuntimeState state() const { return stage; }
    RuntimeResult result() const { return outcome; }
    u32 timerInterrupts() const { return timers; }
    u32 sgiInterrupts() const { return sgis; }
    u32 unexpectedInterrupts() const { return unexpected; }
    u32 lastInterrupt() const { return last; }
    Controller &controller() { return interruptController; }

private:
    RuntimeResult fail(RuntimeResult result);
    Controller interruptController;
    RuntimeClock timer = {};
    u64 period = 0;
    u64 proofBudget = 0;
    u64 started = 0;
    u32 timerInterrupt = 0;
    volatile u64 lastTimer = 0;
    volatile RuntimeState stage = RuntimeState::Empty;
    volatile RuntimeResult outcome = RuntimeResult::NotReady;
    volatile u32 timers = 0;
    volatile u32 sgis = 0;
    volatile u32 unexpected = 0;
    volatile u32 last = 1023;
};

const char *runtimeResultName(RuntimeResult result);

}
#endif
