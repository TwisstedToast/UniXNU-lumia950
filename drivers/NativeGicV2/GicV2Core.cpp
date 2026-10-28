#include "GicV2Core.h"

namespace gicv2 {
namespace {
constexpr Space kDist = Space::Distributor;
constexpr Space kCpu = Space::Cpu;

bool equal(const u8 *bytes, u32 length, const char *name)
{
    u32 index = 0;
    while (index < length && name[index] && bytes[index] == static_cast<u8>(name[index])) ++index;
    return index == length && name[index] == 0;
}
}

u32 compatibleVersion(const void *bytes, u32 length)
{
    if (!bytes || !length || length > 4096) return 0;
    const auto *strings = static_cast<const u8 *>(bytes);
    u32 versions = 0;
    for (u32 start = 0; start < length;) {
        u32 end = start;
        while (end < length && strings[end]) ++end;
        if (end == length || end == start) return 0;
        const u32 size = end - start;
        if (equal(strings + start, size, "arm,gic-2") ||
            equal(strings + start, size, "arm,gic-400") ||
            equal(strings + start, size, "arm,cortex-a15-gic") ||
            equal(strings + start, size, "qcom,msm-qgic2")) versions |= 1;
        if (equal(strings + start, size, "arm,gic-v3")) versions |= 2;
        start = end + 1;
    }
    return versions == 1 ? 2 : versions == 2 ? 3 : 0;
}

u32 interruptId(u32 acknowledge)
{
    return acknowledge & 0x3ff;
}

bool Controller::fail(Error error)
{
    lastError = error;
    return false;
}

u32 Controller::read(Space space, u32 offset) const
{
    const u32 bytes = space == kDist ? distBytes : interfaceBytes;
    if (!io.read32 || bytes < 4 || (offset & 3) || offset > bytes - 4) return 0xffffffffu;
    return io.read32(io.context, space, offset);
}

void Controller::write(Space space, u32 offset, u32 value)
{
    io.write32(io.context, space, offset, value);
    io.barrier(io.context);
}

bool Controller::configure(const RegisterIo &registers, u32 distributorBytes, u32 cpuBytes)
{
    if (bound) return fail(Error::InvalidArgument);
    if (!registers.read32 || !registers.write32 || !registers.barrier ||
        distributorBytes < 0x1000 || cpuBytes < 0x1000) return fail(Error::InvalidArgument);
    io = registers;
    distBytes = distributorBytes;
    interfaceBytes = cpuBytes;
    if (((read(kDist, 0xfe8) >> 4) & 15) != 2) return fail(Error::WrongArchitecture);
    const u32 typer = read(kDist, 4);
    interrupts = ((typer & 31) + 1) * 32;
    securityExtensions = (typer & (1u << 10)) != 0;
    if (interrupts > 1020) interrupts = 1020;
    bound = true;
    lastError = Error::None;
    return true;
}

bool Controller::prepareDistributor(bool requireNonSecure)
{
    if (!bound) return fail(Error::NotConfigured);
    if (distributed) return fail(Error::InvalidArgument);
    write(kCpu, 0, read(kCpu, 0) & ~3u);
    write(kDist, 0, read(kDist, 0) & ~3u);
    if ((read(kDist, 0) & 3) || (read(kCpu, 0) & 3)) return fail(Error::RegisterRejected);
    if (securityExtensions) {
        const u32 original = read(kDist, 0x80);
        write(kDist, 0x80, original ^ 1u);
        secure = ((read(kDist, 0x80) ^ original) & 1) != 0;
        write(kDist, 0x80, original);
    }
    if (requireNonSecure && secure) return fail(Error::SecureAccess);
    u32 targets = 0;
    for (u32 offset = 0x800; offset < 0x820; offset += 4) targets |= read(kDist, offset);
    targets |= targets >> 16;
    targets |= targets >> 8;
    target = static_cast<u8>(targets);
    if (!target || (target & (target - 1))) return fail(Error::InvalidCpuTarget);
    for (u32 id = 32; id < interrupts; id += 32) {
        const u32 bits = interrupts - id >= 32 ? 0xffffffffu : (1u << (interrupts - id)) - 1;
        write(kDist, 0x180 + (id / 32) * 4, bits);
        write(kDist, 0x280 + (id / 32) * 4, bits);
        write(kDist, 0x380 + (id / 32) * 4, bits);
    }
    distributed = true;
    return true;
}

bool Controller::group(u32 id)
{
    if (!securityExtensions) return true;
    const u32 offset = 0x80 + (id / 32) * 4;
    const u32 mask = 1u << (id % 32);
    const u32 before = read(kDist, offset);
    write(kDist, offset, secure ? before & ~mask : before | mask);
    return !secure || !(read(kDist, offset) & mask);
}

bool Controller::priority(u32 id, u8 value)
{
    const u32 offset = 0x400 + (id & ~3u);
    const u32 shift = (id & 3) * 8;
    const u32 before = read(kDist, offset);
    write(kDist, offset, (before & ~(0xffu << shift)) | (static_cast<u32>(value) << shift));
    return ((read(kDist, offset) >> shift) & 0xf8) == (value & 0xf8);
}

bool Controller::prepareLocal(u32 timerInterrupt)
{
    if (!distributed) return fail(Error::NotPrepared);
    if (timerInterrupt < 16 || timerInterrupt > 31) return fail(Error::InvalidArgument);
    write(kDist, 0x180, 0xffffffffu);
    write(kDist, 0x280, 0xffff0000u);
    write(kDist, 0x380, 0xffffffffu);
    for (u32 offset = 0xf10; offset < 0xf20; offset += 4) write(kDist, offset, 0xffffffffu);
    if (!group(0) || !group(timerInterrupt) || !priority(0, 0x80) || !priority(timerInterrupt, 0x80)) {
        return fail(Error::RegisterRejected);
    }
    const u32 timerShift = (timerInterrupt % 16) * 2;
    write(kDist, 0xc04, read(kDist, 0xc04) & ~(3u << timerShift));
    if (read(kDist, 0xc04) & (2u << timerShift)) return fail(Error::RegisterRejected);
    localInterruptMask = 1u | (1u << timerInterrupt);
    local = true;
    return true;
}

bool Controller::enableLocal()
{
    if (!local) return fail(Error::NotPrepared);
    write(kCpu, 4, 0xff);
    write(kCpu, 8, 0);
    write(kDist, 0x100, localInterruptMask);
    write(kDist, 0, read(kDist, 0) | 1u);
    write(kCpu, 0, (read(kCpu, 0) & 0x1e0u) | 1u);
    if ((read(kDist, 0x100) & localInterruptMask) != localInterruptMask ||
        !(read(kDist, 0) & 1) || (read(kCpu, 0) & 0x601) != 1 ||
        (read(kCpu, 4) & 0xf8) != 0xf8) return fail(Error::RegisterRejected);
    return true;
}

void Controller::disableLocal()
{
    if (bound) write(kCpu, 0, read(kCpu, 0) & ~3u);
}

bool Controller::configureSpi(u32 id, bool edge, u8 value)
{
    if (!distributed) return fail(Error::NotPrepared);
    if (id < 32 || id >= interrupts || (value & 0xf8) == 0xf8) return fail(Error::InvalidArgument);
    mask(id);
    if (!group(id) || !priority(id, value)) return fail(Error::RegisterRejected);
    const u32 targetOffset = 0x800 + (id & ~3u);
    const u32 targetShift = (id & 3) * 8;
    write(kDist, targetOffset, (read(kDist, targetOffset) & ~(0xffu << targetShift)) |
                               (static_cast<u32>(target) << targetShift));
    if (((read(kDist, targetOffset) >> targetShift) & 0xff) != target) return fail(Error::RegisterRejected);
    const u32 configOffset = 0xc00 + (id / 16) * 4;
    const u32 configShift = (id % 16) * 2;
    write(kDist, configOffset, (read(kDist, configOffset) & ~(3u << configShift)) |
                               ((edge ? 2u : 0u) << configShift));
    if (((read(kDist, configOffset) >> configShift) & 2) != (edge ? 2u : 0u)) {
        return fail(Error::RegisterRejected);
    }
    const u32 mask = 1u << (id % 32), offset = (id / 32) * 4;
    write(kDist, 0x280 + offset, mask);
    write(kDist, 0x380 + offset, mask);
    return true;
}

bool Controller::enable(u32 id)
{
    if (!local) return fail(Error::NotPrepared);
    if (id >= interrupts) return fail(Error::InvalidArgument);
    const u32 offset = (id / 32) * 4, mask = 1u << (id % 32);
    write(kDist, 0x100 + offset, mask);
    if (!(read(kDist, 0x100 + offset) & mask)) return fail(Error::RegisterRejected);
    return true;
}

bool Controller::mask(u32 id)
{
    if (!bound) return fail(Error::NotConfigured);
    if (id >= interrupts) return fail(Error::InvalidArgument);
    write(kDist, 0x180 + (id / 32) * 4, 1u << (id % 32));
    return true;
}

bool Controller::sendSgi(u8 targets, u8 id)
{
    if (!local) return fail(Error::NotPrepared);
    if (!targets || id >= 16) return fail(Error::InvalidArgument);
    io.barrier(io.context);
    write(kDist, 0xf00, (static_cast<u32>(targets) << 16) | id);
    return true;
}

u32 Controller::acknowledge()
{
    return local ? read(kCpu, 0xc) : 1023;
}

bool Controller::end(u32 rawAcknowledge)
{
    if (!local) return fail(Error::NotPrepared);
    if (interruptId(rawAcknowledge) >= 1020) return fail(Error::InvalidArgument);
    io.barrier(io.context);
    write(kCpu, 0x10, rawAcknowledge);
    return true;
}
}
