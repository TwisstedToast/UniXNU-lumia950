#include "Timing.h"
#include <stdio.h>

namespace {
unsigned checks;
unsigned failures;
void check(bool passed, const char *name)
{
    ++checks;
    if (!passed) { ++failures; fprintf(stderr, "FAIL: %s\n", name); }
}
bool closeTo(double actual, double expected)
{
    const double difference = actual - expected;
    return difference > -0.001 && difference < 0.001;
}
}

int main()
{
    using namespace lumia_demo;
    DisplayRate rate = {};
    check(!updateDisplayRate(rate, {10, 10, 10, 1000}, 1000), "anchor does not claim FPS");
    check(!rate.available, "unmeasured FPS unavailable");
    check(!updateDisplayRate(rate, {10, 10, 10, 1000}, 1000) && rate.anchored,
          "identical statistics do not invalidate the anchor");
    check(!updateDisplayRate(rate, {70, 70, 70, 2000}, 1000), "short window ignored");
    check(updateDisplayRate(rate, {130, 130, 130, 3000}, 1000), "two second window");
    check(closeTo(rate.framesPerSecond, 60) && closeTo(rate.refreshesPerSecond, 60), "60 FPS measured");
    check(updateDisplayRate(rate, {190, 250, 250, 5000}, 1000), "half rate measured");
    check(closeTo(rate.framesPerSecond, 30) && closeTo(rate.refreshesPerSecond, 60), "refresh is not FPS");
    check(!updateDisplayRate(rate, {190, 370, 370, 7000}, 1000) && !rate.available, "stalled presents unavailable");
    check(!updateDisplayRate(rate, {2, 2, 2, 6000}, 1000) && !rate.available, "backward clock resets");
    check(!updateDisplayRate(rate, {500, 3, 3, 8000}, 1000) && !rate.available, "impossible counters refused");
    resetDisplayRate(rate);
    check(!rate.available && !rate.anchored, "explicit disjoint reset");
    check(!updateDisplayRate(rate, {0xfffffff0u, 0xfffffff0u, 0xfffffff0u, 1000}, 1000), "wrap anchor");
    check(updateDisplayRate(rate, {104, 104, 104, 3000}, 1000), "natural counter wrap");
    check(closeTo(rate.framesPerSecond, 60), "wrap FPS");
    check(!updateDisplayRate(rate, {224, 224, 224, 5000}, 0) && !rate.available, "missing frequency");
    check(!updateDisplayRate(rate, {224, 224, 224, 5000}, UINT64_MAX), "frequency overflow");
    updateDisplayRate(rate, {1, 1, 1, 1000}, 1000);
    check(!updateDisplayRate(rate, {121, 121, 121, 22001}, 1000), "stale statistics refused");
    double milliseconds = 0;
    check(validGpuDuration(1000, 3000, 1000000, false, milliseconds), "GPU timestamps valid");
    check(closeTo(milliseconds, 2), "GPU milliseconds");
    check(!validGpuDuration(1000, 3000, 1000000, true, milliseconds), "disjoint GPU clock");
    check(!validGpuDuration(1000, 1000, 1000000, false, milliseconds), "equal timestamps");
    check(!validGpuDuration(3000, 1000, 1000000, false, milliseconds), "backward timestamp");
    check(!validGpuDuration(1000, 3000, 0, false, milliseconds), "zero GPU frequency");
    check(!validGpuDuration(0, 10000001, 1000000, false, milliseconds), "implausible GPU duration");
    printf("GPU demo timing: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
