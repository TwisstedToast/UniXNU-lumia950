#ifndef NATIVE_SDHCI_POLLING_HOST_H
#define NATIVE_SDHCI_POLLING_HOST_H

#include "SdhciCore.h"

namespace sdhci {

struct RegisterIo {
    void *context;
    u8 (*read8)(void *, u32);
    u32 (*read32)(void *, u32);
    void (*write8)(void *, u32, u8);
    void (*write16)(void *, u32, u16);
    void (*write32)(void *, u32, u32);
    void (*delayUs)(void *, u32);
};

struct PollLimits {
    u32 commandPolls = 10000;
    u32 transferPolls = 50000;
    u32 resetPolls = 10000;
    u32 delayUs = 10;
    u32 cardPolls = 100;
};

enum class ResponseType : u8 { None, R1, R1Busy, R2 };
enum class HostError : u8 {
    None, InvalidArgument, NotReady, ResetTimeout, InhibitTimeout,
    CommandTimeout, DataTimeout, Controller, CardStatus, CardState
};
enum class HostPhase : u8 { Idle, Reset, Inhibit, Command, Buffer, Transfer, Card };

struct HostResult {
    HostError error = HostError::None;
    HostPhase phase = HostPhase::Idle;
    u32 interruptStatus = 0;
    u32 cardStatus = 0;
    u32 bytesTransferred = 0;
    u8 command = 0;
};

class PollingHost {
public:
    bool configure(const RegisterIo &registers, const PollLimits &pollLimits = {});
    bool start();
    void stop();
    bool command(u8 index, u32 argument, ResponseType type, u32 response[4]);
    bool read512(u8 index, u32 argument, u8 *destination);
    bool waitCardReady(u16 relativeAddress, u32 state);
    bool ready() const { return running; }
    const HostResult &result() const { return last; }

private:
    bool issue(u8 index, u32 argument, ResponseType type, bool data, u32 response[4]);
    bool waitStatus(u32 wanted, u32 &remaining, HostPhase phase, HostError timeout);
    bool fail(HostError error, HostPhase phase, u32 status = 0);
    void acknowledge(u32 status);
    bool validIo() const;

    RegisterIo io = {};
    PollLimits limits = {};
    HostResult last = {};
    bool running = false;
};

enum class WarmCardError : u8 {
    None, InvalidArgument, Host, NoRelativeAddress, UnexpectedState,
    InvalidCsd, InvalidExtCsd, UnsupportedCard, WrongPartition, CommandQueue
};

struct WarmCardInfo {
    u16 relativeAddress = 0;
    CsdInfo csd = {};
    ExtCsdInfo extCsd = {};
    u64 capacityBytes = 0;
};

class WarmCard {
public:
    bool attach(PollingHost &controller, const u16 *candidates, u32 count);
    bool readBlock(u64 lba, u8 *destination);
    void detach();
    bool ready() const { return attached && host && host->ready(); }
    const WarmCardInfo &info() const { return card; }
    WarmCardError error() const { return lastError; }

private:
    bool fail(WarmCardError error);
    PollingHost *host = nullptr;
    WarmCardInfo card = {};
    WarmCardError lastError = WarmCardError::None;
    bool attached = false;
};

const char *hostErrorName(HostError error);

}

#endif
