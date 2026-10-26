#include "../../drivers/NativeGicV2/GicV2Core.h"

using u64 = unsigned long long;
using usize = __SIZE_TYPE__;
using namespace gicv2;

extern "C" void *memset(void *destination, int value, usize count)
{
    auto *bytes = static_cast<volatile u8 *>(destination);
    for (usize index = 0; index < count; ++index) bytes[index] = static_cast<u8>(value);
    return destination;
}

extern "C" void *memcpy(void *destination, const void *source, usize count)
{
    auto *output = static_cast<u8 *>(destination);
    const auto *input = static_cast<const u8 *>(source);
    for (usize index = 0; index < count; ++index) output[index] = input[index];
    return destination;
}

namespace {
Controller *interruptController;
volatile u32 timers;
volatile u32 sgis;
volatile u32 spis;

void print(const char *message)
{
    auto *uart = reinterpret_cast<volatile u32 *>(0x09000000ULL);
    while (*message) {
        while (uart[0x18 / 4] & 0x20) asm volatile("yield");
        uart[0] = static_cast<u8>(*message++);
    }
}

void hex(u64 value)
{
    const char *digits = "0123456789abcdef";
    char output[19];
    output[0] = '0';
    output[1] = 'x';
    for (u32 index = 0; index < 16; ++index) output[2 + index] = digits[(value >> (60 - 4 * index)) & 15];
    output[18] = 0;
    print(output);
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
    const u64 base = space == Space::Distributor ? 0x08000000ULL : 0x08010000ULL;
    asm volatile("dsb sy" ::: "memory");
    const u32 value = *reinterpret_cast<volatile u32 *>(base + offset);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

void write(void *, Space space, u32 offset, u32 value)
{
    const u64 base = space == Space::Distributor ? 0x08000000ULL : 0x08010000ULL;
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile u32 *>(base + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

void barrier(void *)
{
    asm volatile("dsb sy; isb" ::: "memory");
}

u64 counter()
{
    u64 value;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
    return value;
}
}

extern "C" void gic_test_irq()
{
    for (u32 count = 0; count < 64; ++count) {
        const u32 raw = interruptController->acknowledge();
        const u32 id = interruptId(raw);
        if (id >= 1020) return;
        if (id == 27) {
            asm volatile("msr cntv_ctl_el0, xzr; isb" ::: "memory");
            timers = timers + 1;
        } else if (id == 0) sgis = sgis + 1;
        else if (id == 40) spis = spis + 1;
        else {
            hex(raw);
            fail("unexpected INTID");
        }
        if (!interruptController->end(raw)) fail("EOIR");
    }
    fail("interrupt storm");
}

extern "C" [[noreturn]] void gic_test_exception()
{
    u64 syndrome, address;
    asm volatile("mrs %0, esr_el1; mrs %1, elr_el1" : "=r"(syndrome), "=r"(address));
    print("exception ESR=");
    hex(syndrome);
    print(" ELR=");
    hex(address);
    fail("exception vector");
}

extern "C" [[noreturn]] void gic_test_main()
{
    u64 currentEl, frequency;
    asm volatile("mrs %0, CurrentEL; mrs %1, cntfrq_el0" : "=r"(currentEl), "=r"(frequency));
    print("GICV2 IRQ TEST EL=");
    hex(currentEl >> 2);
    print("\n");
    if (currentEl != 4 || !frequency) fail("expected EL1 and live timer");
    Controller controller;
    interruptController = &controller;
    const RegisterIo registers = {nullptr, read, write, barrier};
    if (!controller.configure(registers, 0x1000, 0x1000)) fail("configure");
    if (!controller.prepareDistributor()) {
        print("CORE ERROR=");
        hex(static_cast<u32>(controller.error()));
        print(" CTLR=");
        hex(controller.read(Space::Distributor, 0));
        print(" TARGET=");
        hex(controller.read(Space::Distributor, 0x800));
        print(" GICC=");
        hex(controller.read(Space::Cpu, 0));
        fail("distributor");
    }
    if (!controller.prepareLocal()) fail("banked local interrupts");
    if (!controller.enableLocal()) {
        print(" ENABLE CTLR/DIST/LOCAL/PMR ");
        hex(controller.read(Space::Cpu, 0));
        hex(controller.read(Space::Distributor, 0));
        hex(controller.read(Space::Distributor, 0x100));
        hex(controller.read(Space::Cpu, 4));
        fail("GICC enable");
    }
    const bool security = (read(nullptr, Space::Distributor, 4) & (1u << 10)) != 0;
    print(security ? "SECURITY EXTENSIONS ON\n" : "SECURITY EXTENSIONS OFF\n");
    if (security && !controller.secureView()) {
        if (controller.configureSpi(41, true)) fail("secure-owned SPI accepted");
        print("SECURE SPI REFUSED\n");
    }
    if (!controller.configureSpi(40, true) || !controller.enable(40)) fail("SPI route");
    asm volatile("msr daifclr, #2; isb" ::: "memory");
    for (u32 round = 1; round <= 2; ++round) {
        const u64 ticks = frequency / 1000;
        const u64 enabled = 1;
        asm volatile("msr cntv_tval_el0, %0; msr cntv_ctl_el0, %1; isb" ::
                     "r"(ticks), "r"(enabled) : "memory");
        if (!controller.sendSgi(controller.cpuTarget())) fail("SGIR");
        write(nullptr, Space::Distributor, 0x204, 1u << 8);
        const u64 started = counter();
        while (timers < round || sgis < round || spis < round) {
            if (counter() - started > frequency * 2) {
                print(" COUNTS "); hex(timers); hex(sgis); hex(spis);
                print(" HPPIR "); hex(controller.read(Space::Cpu, 0x18));
                print(" PENDING "); hex(controller.read(Space::Distributor, 0x200));
                print(" PENDING1 "); hex(controller.read(Space::Distributor, 0x204));
                u64 timer, mask, status;
                asm volatile("mrs %0, cntv_ctl_el0; mrs %1, daif; mrs %2, isr_el1" :
                             "=r"(timer), "=r"(mask), "=r"(status));
                print(" TIMER DAIF ISR "); hex(timer); hex(mask); hex(status);
                fail("IRQ deadline");
            }
            asm volatile("yield" ::: "memory");
        }
    }
    controller.disableLocal();
    print("PASS ASYNC TIMER=2 SGI=2 SPI=2 EOIMODE=0\n");
    finish(0);
}
