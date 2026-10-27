#ifndef LUMIA_GPU_DEMO_TIMING_H
#define LUMIA_GPU_DEMO_TIMING_H

#include <stdint.h>

namespace lumia_demo {

struct DisplaySample {
    uint32_t presentCount;
    uint32_t presentRefreshCount;
    uint32_t syncRefreshCount;
    uint64_t syncTicks;
};

struct DisplayRate {
    bool anchored;
    bool available;
    DisplaySample anchor;
    double framesPerSecond;
    double refreshesPerSecond;
    uint32_t windows;
};

inline void resetDisplayRate(DisplayRate &rate)
{
    rate.anchored = false;
    rate.available = false;
    rate.framesPerSecond = 0;
    rate.refreshesPerSecond = 0;
}

inline bool updateDisplayRate(DisplayRate &rate, const DisplaySample &sample, uint64_t frequency)
{
    if (!frequency || frequency > UINT64_MAX / 20) {
        resetDisplayRate(rate);
        return false;
    }
    if (!rate.anchored) {
        rate.anchor = sample;
        rate.anchored = true;
        return false;
    }
    const uint32_t frames = sample.presentCount - rate.anchor.presentCount;
    const uint32_t refreshes = sample.syncRefreshCount - rate.anchor.syncRefreshCount;
    const uint32_t presentRefreshes = sample.presentRefreshCount - rate.anchor.presentRefreshCount;
    if (sample.syncTicks == rate.anchor.syncTicks && !frames && !refreshes && !presentRefreshes) {
        return false;
    }
    if (sample.syncTicks <= rate.anchor.syncTicks || frames > 0x7fffffffu ||
        refreshes > 0x7fffffffu || presentRefreshes > 0x7fffffffu) {
        resetDisplayRate(rate);
        rate.anchor = sample;
        rate.anchored = true;
        return false;
    }
    const uint64_t elapsed = sample.syncTicks - rate.anchor.syncTicks;
    if (elapsed < frequency * 2) { return false; }
    if (!frames || !refreshes || !presentRefreshes || elapsed > frequency * 20 ||
        frames > refreshes + 1 || frames > presentRefreshes + 1) {
        resetDisplayRate(rate);
        rate.anchor = sample;
        rate.anchored = true;
        return false;
    }
    rate.framesPerSecond = static_cast<double>(frames) * static_cast<double>(frequency) /
        static_cast<double>(elapsed);
    rate.refreshesPerSecond = static_cast<double>(refreshes) * static_cast<double>(frequency) /
        static_cast<double>(elapsed);
    rate.available = true;
    rate.anchor = sample;
    ++rate.windows;
    return true;
}

inline bool validGpuDuration(uint64_t begin, uint64_t end, uint64_t frequency,
                             bool disjoint, double &milliseconds)
{
    if (disjoint || !frequency || end <= begin) { return false; }
    milliseconds = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
    return milliseconds > 0 && milliseconds < 10000;
}

}
#endif
