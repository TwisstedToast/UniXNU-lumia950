#include "SmmuRoutes.h"

namespace adreno {
namespace smmu {
namespace {

bool read(const RegisterIo &io, u32 offset, u32 &value)
{
    return io.read(io.context, RegisterSpace::Smmu, offset, &value) && value != 0xffffffffu;
}

bool validMapping(u32 value, u32 context)
{
    const u32 privilege = (value >> 24) & 3;
    return (value & 0x30000) == 0 && (value & 255) == context &&
        (context == 0 ? (privilege == 0 || privilege == 2) : (privilege == 0 || privilege == 3));
}

}

RouteResult inspectRoutes(Routes &routes, const RegisterIo &io,
    const RouteContract &contract, bool contextsEnabled)
{
    routes.qualified = false;
    if (!contract.platformQualified || !contract.exclusiveOwnership || !contract.nonSecureEl1) {
        return RouteResult::Unqualified;
    }
    if (!io.read) { return RouteResult::Invalid; }
    GpuSnapshot snapshot = {};
    if (readSnapshot(io, &snapshot) != ProbeResult::Ready ||
        (snapshot.status & kGpuBusy) || (snapshot.meControl & kMeHalt) == 0 ||
        (snapshot.interruptStatus & kGpuFaultMask)) { return RouteResult::NotReady; }
    routes.io = io;
    routes.contract = contract;
    if (!read(io, 0x20, routes.id[0]) || !read(io, 0x24, routes.id[1]) ||
        !read(io, 0x28, routes.id[2]) || !read(io, 0, routes.control)) { return RouteResult::IoError; }
    const u32 id0 = routes.id[0];
    const u32 id1 = routes.id[1];
    if ((id0 & (1u << 30)) == 0 || (id0 & (1u << 26)) == 0 || (id0 & (1u << 25)) ||
        (id1 & (1u << 31)) || ((id1 >> 28) & 7) != 2 ||
        (id1 & 255) < 2 || ((id1 >> 16) & 255) != 0) { return RouteResult::Unsupported; }
    if ((routes.control & 1) || (routes.control & (1u << 10)) == 0) { return RouteResult::NotReady; }
    u32 fault = 0;
    if (!read(io, 0x48, fault)) { return RouteResult::IoError; }
    if (fault) { return RouteResult::NotReady; }
    const bool matching = (id0 & (1u << 27)) != 0;
    const u32 streamBits = (id0 >> 9) & 15;
    routes.count = matching ? id0 & 255 : (streamBits < 8 ? 1u << streamBits : 0);
    if (routes.count < 2 || routes.count > kMaximumStreams) { return RouteResult::Unsupported; }
    bool found[2] = {};
    for (u32 index = 0; index < routes.count; ++index) {
        routes.match[index] = 0;
        if ((matching && !read(io, 0x800 + index * 4, routes.match[index])) ||
            !read(io, 0xc00 + index * 4, routes.mapping[index])) { return RouteResult::IoError; }
        const u32 match = routes.match[index];
        if (matching && (match & (1u << 31)) == 0) { continue; }
        const u32 stream = matching ? match & 0xffff : index;
        const u32 mask = matching ? (match >> 16) & 0x7fff : 0;
        bool used = false;
        for (u32 target = 0; target < 2; ++target) {
            if ((target & ~mask) != (stream & ~mask)) { continue; }
            if (mask || stream != target || found[target] || !validMapping(routes.mapping[index], target)) {
                return RouteResult::Mismatch;
            }
            found[target] = true;
            used = true;
        }
        if (!used && (routes.mapping[index] & 0x30000) == 0 &&
            (routes.mapping[index] & 255) < 2) { return RouteResult::Mismatch; }
    }
    if (!found[0] || !found[1]) { return RouteResult::Mismatch; }
    for (u32 index = 0; index < 2; ++index) {
        u32 control = 0;
        u32 contextFault = 0;
        if (!read(io, 0x1000 + index * 4, routes.attributes[index]) ||
            !read(io, 0x8000 + index * 0x1000, control) ||
            !read(io, 0x8058 + index * 0x1000, contextFault)) { return RouteResult::IoError; }
        if (((routes.attributes[index] >> 16) & 3) != 1 ||
            ((control & 1) != 0) != contextsEnabled || contextFault) { return RouteResult::NotReady; }
    }
    routes.qualified = true;
    return RouteResult::Ok;
}

RouteResult revalidateRoutes(const Routes &routes, bool contextsEnabled)
{
    if (!routes.qualified || routes.count < 2 || routes.count > kMaximumStreams) {
        return RouteResult::Unqualified;
    }
    Routes observed = {};
    const RouteResult result = inspectRoutes(observed, routes.io, routes.contract, contextsEnabled);
    if (result != RouteResult::Ok) { return result; }
    if (observed.count != routes.count || observed.control != routes.control) { return RouteResult::Mismatch; }
    for (u32 index = 0; index < 3; ++index) {
        if (observed.id[index] != routes.id[index]) { return RouteResult::Mismatch; }
    }
    for (u32 index = 0; index < routes.count; ++index) {
        if (observed.match[index] != routes.match[index] || observed.mapping[index] != routes.mapping[index]) {
            return RouteResult::Mismatch;
        }
    }
    for (u32 index = 0; index < 2; ++index) {
        if (observed.attributes[index] != routes.attributes[index]) { return RouteResult::Mismatch; }
    }
    return RouteResult::Ok;
}

}
}
