#ifndef NATIVE_GIC_V2_CORE_H
#define NATIVE_GIC_V2_CORE_H

namespace gicv2 {
using u8 = unsigned char;
using u32 = unsigned int;
static_assert(sizeof(u8) == 1 && sizeof(u32) == 4, "GIC register widths");

enum class Space : u8 { Distributor, Cpu };
enum class Error : u8 {
    None, InvalidArgument, NotConfigured, WrongArchitecture,
    InvalidCpuTarget, RegisterRejected, NotPrepared, SecureAccess
};

struct RegisterIo {
    void *context;
    u32 (*read32)(void *, Space, u32);
    void (*write32)(void *, Space, u32, u32);
    void (*barrier)(void *);
};

enum class Check : u8 { None, Readback, PriorityPrecision };
enum class Identity : u8 { Unknown, CpuInterface, CpuAndDistributor };
struct RegisterFailure {
    Check check = Check::None;
    Space space = Space::Distributor;
    u32 offset = 0;
    u32 expected = 0;
    u32 observed = 0;
    u32 mask = 0;
};

u32 compatibleVersion(const void *bytes, u32 length);
u32 interruptId(u32 acknowledge);

class Controller {
public:
    bool configure(const RegisterIo &registers, u32 distributorBytes, u32 cpuBytes);
    bool prepareDistributor(bool requireNonSecure = false);
    bool prepareLocal(u32 timerInterrupt = 27);
    bool enableLocal();
    void disableLocal();
    bool configureSpi(u32 id, bool edge, u8 priority = 0xa0);
    bool enable(u32 id);
    bool mask(u32 id);
    bool sendSgi(u8 targets, u8 id = 0);
    u32 acknowledge();
    bool end(u32 rawAcknowledge);
    u32 read(Space space, u32 offset) const;
    u32 interruptCount() const { return interrupts; }
    u8 cpuTarget() const { return target; }
    bool secureView() const { return secure; }
    Error error() const { return lastError; }
    RegisterFailure failure() const { return registerFailure; }
    u32 priorityBits() const { return interruptPriorityBits; }
    u32 priorityMaskBits() const { return cpuPriorityBits; }
    bool priorityMaskEnabled() const;
    Identity identity() const { return identitySource; }
    u32 cpuInterfaceId() const { return interfaceIdentity; }
    u32 distributorPeripheralId() const { return peripheralIdentity; }

private:
    bool fail(Error error);
    bool group(u32 id);
    bool priority(u32 id, u8 value);
    bool probePriority(Space space, u32 offset, u32 shift, u32 &bits);
    bool verify(Space space, u32 offset, u32 expected, u32 mask = 0xffffffffu);
    bool verifyObserved(Space space, u32 offset, u32 expected, u32 observed, u32 mask);
    void write(Space space, u32 offset, u32 value);
    RegisterIo io = {};
    u32 distBytes = 0;
    u32 interfaceBytes = 0;
    u32 interrupts = 0;
    u32 localInterruptMask = 0;
    u32 interruptPriorityBits = 0;
    u32 cpuPriorityBits = 0;
    u32 interfaceIdentity = 0;
    u32 peripheralIdentity = 0;
    Identity identitySource = Identity::Unknown;
    RegisterFailure registerFailure;
    u8 target = 0;
    bool secure = false;
    bool securityExtensions = false;
    bool bound = false;
    bool distributed = false;
    bool local = false;
    Error lastError = Error::None;
};
}

#endif
