#include "../FirmwareSmem.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
unsigned checks;
unsigned failures;

void Expect(bool condition, const char *label)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

struct Environment {
    static Environment *active;
    EFI_BOOT_SERVICES services = {};
    struct SmemProtocol {
        UINT32 (EFIAPI *read)(UINT32, VOID *, INT32);
        UINT32 (EFIAPI *readOffset)(UINT32, VOID *, INT32, INT32);
        VOID *(EFIAPI *get)(UINT32, UINT32 *);
    } protocol = {Read, nullptr, Get};
    std::array<UINT32, 64 * 8> channels = {};
    EFI_STATUS locateStatus = EFI_SUCCESS;
    UINT32 readStatus = 0;
    UINT32 controlSize = 88;
    UINT32 fifoSize = 2048;
    UINTN controlAddress = 0x06a10000;
    UINTN fifoAddress = 0x06a20000;
    UINT32 requestedControl = 0;
    UINT32 requestedFifo = 0;
    unsigned locateCalls = 0;
    unsigned readCalls = 0;
    unsigned getCalls = 0;
    bool nullProtocol = false;

    Environment()
    {
        active = this;
        services.LocateProtocol = Locate;
        Channel(4, 9, 0x10f, 1);
    }

    void Channel(UINT32 slot, UINT32 id, UINT32 type, UINT32 references)
    {
        channels[slot * 8 + 5] = id;
        channels[slot * 8 + 6] = type;
        channels[slot * 8 + 7] = references;
    }

    static EFI_STATUS EFIAPI Locate(EFI_GUID *guid, VOID *registration, VOID **output)
    {
        Environment &environment = *active;
        ++environment.locateCalls;
        const EFI_GUID expected = {0xe738ff9d, 0x020f, 0x4ecd, {0x87,0xfc,0xd5,0x82,0x50,0x75,0xaa,0xcd}};
        Expect(guid && std::memcmp(guid, &expected, sizeof(expected)) == 0 && !registration,
               "only the Qcom SMEM protocol is requested");
        if (EFI_ERROR(environment.locateStatus)) { return environment.locateStatus; }
        *output = environment.nullProtocol ? nullptr : &environment.protocol;
        return EFI_SUCCESS;
    }

    static UINT32 EFIAPI Read(UINT32 item, VOID *destination, INT32 length)
    {
        Environment &environment = *active;
        ++environment.readCalls;
        Expect(item == 13 && length == 2048 && (reinterpret_cast<UINTN>(destination) & 63) == 0,
               "only the aligned, bounded channel table is copied");
        if (environment.readStatus) { return environment.readStatus; }
        std::memcpy(destination, environment.channels.data(), environment.channels.size() * sizeof(UINT32));
        return 0;
    }

    static VOID *EFIAPI Get(UINT32 item, UINT32 *size)
    {
        Environment &environment = *active;
        ++environment.getCalls;
        if (environment.getCalls == 1) {
            environment.requestedControl = item;
            *size = environment.controlSize;
            return reinterpret_cast<VOID *>(environment.controlAddress);
        }
        environment.requestedFifo = item;
        *size = environment.fifoSize;
        return reinterpret_cast<VOID *>(environment.fifoAddress);
    }
};
Environment *Environment::active = nullptr;

void TestSuccess()
{
    Environment environment;
    const ProbeFirmwareSmem result = ProbeFindFirmwareSmem(&environment.services);
    Expect(result.status == EFI_SUCCESS && result.stage == ProbeSmemStage::Complete,
           "one well-formed APPS-RPM channel is reported");
    Expect(result.protocolPresent && result.channelCount == 1 && result.channelId == 9 &&
           result.channelType == 0x10f && result.referenceCount == 1,
           "channel metadata and upper type bits are preserved");
    Expect(result.controlAddress == 0x06a10000 && result.controlBytes == 88 &&
           result.fifoAddress == 0x06a20000 && result.fifoBytes == 2048,
           "firmware-returned item addresses and sizes are reported");
    Expect(environment.locateCalls == 1 && environment.readCalls == 1 && environment.getCalls == 2,
           "discovery performs only one lookup, one table read, and two item lookups");
    Expect(environment.requestedControl == 23 && environment.requestedFifo == 347,
           "the control and FIFO item IDs derive from the selected channel ID");
    Expect(std::strcmp(ProbeSmemStageName(result.stage), "COMPLETE") == 0,
           "the successful stage has a printable label");
}

void TestProtocolFailures()
{
    ProbeFirmwareSmem result = ProbeFindFirmwareSmem(nullptr);
    Expect(result.status == EFI_INVALID_PARAMETER && result.stage == ProbeSmemStage::Arguments,
           "missing boot services are rejected");
    Environment missingFunction;
    missingFunction.services.LocateProtocol = nullptr;
    result = ProbeFindFirmwareSmem(&missingFunction.services);
    Expect(result.status == EFI_INVALID_PARAMETER && !missingFunction.locateCalls,
           "missing LocateProtocol is rejected without a call");
    Environment missing;
    missing.locateStatus = EFI_NOT_FOUND;
    result = ProbeFindFirmwareSmem(&missing.services);
    Expect(result.status == EFI_NOT_FOUND && !result.protocolPresent && !missing.readCalls,
           "a missing firmware protocol is reported without reading memory");
    Environment nullProtocol;
    nullProtocol.nullProtocol = true;
    result = ProbeFindFirmwareSmem(&nullProtocol.services);
    Expect(result.status == EFI_PROTOCOL_ERROR && !result.protocolPresent,
           "a successful lookup with a null protocol is rejected");
    Environment noRead;
    noRead.protocol.read = nullptr;
    result = ProbeFindFirmwareSmem(&noRead.services);
    Expect(result.status == EFI_PROTOCOL_ERROR && !noRead.readCalls && !noRead.getCalls,
           "a protocol without its read function is rejected");
    Environment noGet;
    noGet.protocol.get = nullptr;
    result = ProbeFindFirmwareSmem(&noGet.services);
    Expect(result.status == EFI_PROTOCOL_ERROR && !noGet.readCalls,
           "a protocol without its item lookup is rejected");
    Environment readFailure;
    readFailure.readStatus = 1;
    result = ProbeFindFirmwareSmem(&readFailure.services);
    Expect(result.status == EFI_DEVICE_ERROR && result.stage == ProbeSmemStage::ReadTable &&
           readFailure.getCalls == 0, "a failed firmware table read stops discovery");
}

void TestChannelFailures()
{
    Environment missing;
    missing.channels.fill(0);
    ProbeFirmwareSmem result = ProbeFindFirmwareSmem(&missing.services);
    Expect(result.status == EFI_NOT_FOUND && !result.channelCount && !missing.getCalls,
           "no APPS-RPM channel is reported without guessing an ID");
    Environment duplicate;
    duplicate.Channel(12, 10, 0x0f, 1);
    result = ProbeFindFirmwareSmem(&duplicate.services);
    Expect(result.status == EFI_COMPROMISED_DATA && result.channelCount == 2 && !duplicate.getCalls,
           "ambiguous APPS-RPM channels are not silently selected");
    Environment badId;
    badId.Channel(4, 64, 0x0f, 1);
    result = ProbeFindFirmwareSmem(&badId.services);
    Expect(result.status == EFI_COMPROMISED_DATA && result.channelId == 64 && !badId.getCalls,
           "out-of-range channel IDs never become SMEM item IDs");
    Environment otherType;
    otherType.Channel(6, 2, 0x0e, 1);
    result = ProbeFindFirmwareSmem(&otherType.services);
    Expect(result.status == EFI_SUCCESS && result.channelId == 9 && result.channelCount == 1,
           "other channel types do not create false ambiguity");
}

void TestLayoutFailures()
{
    Environment missingControl;
    missingControl.controlAddress = 0;
    ProbeFirmwareSmem result = ProbeFindFirmwareSmem(&missingControl.services);
    Expect(result.status == EFI_NOT_FOUND && result.stage == ProbeSmemStage::FindItems,
           "missing control item is reported");
    Environment missingFifo;
    missingFifo.fifoAddress = 0;
    result = ProbeFindFirmwareSmem(&missingFifo.services);
    Expect(result.status == EFI_NOT_FOUND && result.stage == ProbeSmemStage::FindItems,
           "missing FIFO item is reported");
    Environment shortControl;
    shortControl.controlSize = 84;
    result = ProbeFindFirmwareSmem(&shortControl.services);
    Expect(result.status == EFI_COMPROMISED_DATA && result.stage == ProbeSmemStage::ValidateLayout,
           "a truncated two-half control block is rejected");
    Environment unaligned;
    unaligned.fifoAddress += 1;
    result = ProbeFindFirmwareSmem(&unaligned.services);
    Expect(result.status == EFI_COMPROMISED_DATA, "unaligned FIFO pointers are rejected");
    Environment smallFifo;
    smallFifo.fifoSize = 1016;
    result = ProbeFindFirmwareSmem(&smallFifo.services);
    Expect(result.status == EFI_COMPROMISED_DATA, "undersized FIFO allocation is rejected");
    Environment oddFifo;
    oddFifo.fifoSize = 2052;
    result = ProbeFindFirmwareSmem(&oddFifo.services);
    Expect(result.status == EFI_COMPROMISED_DATA, "an uneven FIFO split is rejected");
    Environment nonPowerOfTwo;
    nonPowerOfTwo.fifoSize = 3072;
    result = ProbeFindFirmwareSmem(&nonPowerOfTwo.services);
    Expect(result.status == EFI_COMPROMISED_DATA, "a non-power-of-two FIFO half is rejected");
    Environment hugeFifo;
    hugeFifo.fifoSize = 262144;
    result = ProbeFindFirmwareSmem(&hugeFifo.services);
    Expect(result.status == EFI_COMPROMISED_DATA, "an oversized FIFO allocation is rejected");
    Environment overlapping;
    overlapping.fifoAddress = overlapping.controlAddress + 64;
    result = ProbeFindFirmwareSmem(&overlapping.services);
    Expect(result.status == EFI_COMPROMISED_DATA, "overlapping control and FIFO allocations are rejected");
    Environment overflow;
    overflow.controlAddress = ~static_cast<UINTN>(3);
    result = ProbeFindFirmwareSmem(&overflow.services);
    Expect(result.status == EFI_COMPROMISED_DATA, "address arithmetic overflow is rejected");
    Environment validMaximum;
    validMaximum.fifoSize = 131072;
    validMaximum.fifoAddress = 0x07000000;
    result = ProbeFindFirmwareSmem(&validMaximum.services);
    Expect(result.status == EFI_SUCCESS, "the maximum supported FIFO split is accepted");
}
}

int main()
{
    TestSuccess();
    TestProtocolFailures();
    TestChannelFailures();
    TestLayoutFailures();
    std::printf("Firmware SMEM discovery: %u checks, %u failures (mock protocol; no hardware writes)\n",
                checks, failures);
    return failures ? 1 : 0;
}
