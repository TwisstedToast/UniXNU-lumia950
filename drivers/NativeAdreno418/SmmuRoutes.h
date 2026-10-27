#ifndef NATIVE_ADRENO418_SMMU_ROUTES_H
#define NATIVE_ADRENO418_SMMU_ROUTES_H

#include "GpuIo.h"

namespace adreno {
namespace smmu {

constexpr u32 kMaximumStreams = 128;
struct RouteContract {
    bool platformQualified;
    bool exclusiveOwnership;
    bool nonSecureEl1;
};
enum class RouteResult : u32 { Ok, Invalid, Unqualified, NotReady, IoError, Unsupported, Mismatch };
struct Routes {
    RegisterIo io;
    RouteContract contract;
    u32 id[3];
    u32 control;
    u32 count;
    u32 match[kMaximumStreams];
    u32 mapping[kMaximumStreams];
    u32 attributes[2];
    bool qualified;
};

RouteResult inspectRoutes(Routes &routes, const RegisterIo &io,
    const RouteContract &contract, bool contextsEnabled = false);
RouteResult revalidateRoutes(const Routes &routes, bool contextsEnabled);

}
}
#endif
