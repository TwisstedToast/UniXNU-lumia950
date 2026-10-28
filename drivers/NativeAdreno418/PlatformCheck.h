#ifndef NATIVE_ADRENO418_PLATFORM_CHECK_H
#define NATIVE_ADRENO418_PLATFORM_CHECK_H
#include "AdrenoTypes.h"
namespace adreno {
constexpr u32 kLumia950VirtualTimerInterrupt = 20;
bool isLumia950Madt(const u8 *table, u32 bytes);
bool isLumia950Gtdt(const u8 *table, u32 bytes);
}
#endif
