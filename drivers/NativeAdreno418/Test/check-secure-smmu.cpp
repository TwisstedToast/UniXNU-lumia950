#include "SecureSmmu.h"
#include "BoardConstants.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

using namespace adreno;
using namespace adreno::secure;

namespace {
unsigned checks;
unsigned failures;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", message); }
}

enum class Fault {
    None, AvailabilityTrap, AvailabilityUnsupported, AvailabilityZero, AvailabilityWide,
    RestoreTrap, RestoreUnsupported, RestoreMonitor, RestoreService, QueryResume,
    RestoreResume, ForeverResume, Busy
};

struct Device {
    Fault fault = Fault::None;
    Configuration configuration = {};
    std::vector<std::array<u64, 8>> calls;
    std::array<u32, 16384> mmss = {};
    std::array<u32, 16384> gpu = {};
    std::array<u32, 2048> contexts = {};
    u32 smmuReads = 0;
    u32 failedRead = 0;
    u32 writes = 0;
    bool violation = false;
    bool resumePending = false;
    bool resumingRestore = false;
    std::array<u64, 8> suspendedRequest = {};
    static constexpr Contract contract = {true, true, true, true, true};

    Device()
    {
        mmss[0x4024 / 4] = mmss[0x4034 / 4] = 0xa0282000;
        mmss[0x4028 / 4] = 0x6ff1;
        mmss[0x403c / 4] = 0x20008001;
        mmss[0x40b0 / 4] = 1;
        gpu[0] = 0x51;
        gpu[2] = 0x0229249a;
        gpu[0x22d] = kMeHalt;
    }

    static bool invoke(void *opaque, MonitorRegisters *registers)
    {
        auto &device = *static_cast<Device *>(opaque);
        std::array<u64, 8> request = {};
        for (u32 index = 0; index < 8; ++index) { request[index] = registers->values[index]; }
        device.calls.push_back(request);
        bool restore = request[0] == kRestoreConfiguration;
        if (device.resumePending) {
            if (request[0] != 1 || request[6] != 0x4188992) { device.violation = true; return false; }
            for (u32 index : {1u, 2u, 3u, 4u, 5u, 7u}) {
                if (request[index] != device.suspendedRequest[index]) { device.violation = true; return false; }
            }
            restore = device.resumingRestore;
            device.resumePending = false;
        } else if (restore) {
            if (request[1] != 2 || request[2] != 18 || request[3]) { device.violation = true; return false; }
        } else if (request[0] != kCallAvailable || request[1] != 1 || request[2] != kRestoreConfiguration) {
            device.violation = true;
            return false;
        }
        if (!request[0] || (request[0] != 1 && (request[4] || request[5] || request[6] || request[7]))) {
            device.violation = true;
            return false;
        }
        if ((!restore && device.fault == Fault::AvailabilityTrap) ||
            (restore && device.fault == Fault::RestoreTrap)) { return false; }
        if (device.fault == Fault::ForeverResume ||
            (device.fault == Fault::QueryResume && device.calls.size() == 1) ||
            (device.fault == Fault::RestoreResume && device.calls.size() == 2)) {
            registers->values[0] = 1;
            for (u32 index : {1u, 2u, 3u, 4u, 5u, 7u}) { registers->values[index] = 0xdeadbeef; }
            registers->values[6] = 0x4188992;
            device.suspendedRequest = request;
            device.resumePending = true;
            device.resumingRestore = restore;
            return true;
        }
        registers->values[0] = 0;
        registers->values[1] = restore ? 0 : 1;
        if ((!restore && device.fault == Fault::AvailabilityUnsupported) ||
            (restore && device.fault == Fault::RestoreUnsupported)) { registers->values[0] = 0xffffffffu; }
        if (!restore && device.fault == Fault::AvailabilityZero) { registers->values[1] = 0; }
        if (!restore && device.fault == Fault::AvailabilityWide) { registers->values[1] = (1ull << 32) | 1; }
        if (restore && device.fault == Fault::RestoreMonitor) { registers->values[0] = 0xfffffffeu; }
        if (restore && device.fault == Fault::Busy) { registers->values[0] = 0xfffffff4u; }
        if (restore && device.fault == Fault::RestoreService) { registers->values[1] = 1; }
        return true;
    }

    static bool read(void *opaque, RegisterSpace space, u32 offset, u32 *value)
    {
        auto &device = *static_cast<Device *>(opaque);
        if (!value || (offset & 3) || offset >= 0x10000) { device.violation = true; return false; }
        if (space == RegisterSpace::Mmss) { *value = device.mmss[offset / 4]; return true; }
        if (space == RegisterSpace::Gpu) { *value = device.gpu[offset / 4]; return true; }
        if (offset < 0x8000 || offset >= 0xa000) { device.violation = true; return false; }
        ++device.smmuReads;
        if (device.smmuReads == device.failedRead) { return false; }
        *value = device.contexts[(offset - 0x8000) / 4];
        return true;
    }

    static bool write(void *opaque, RegisterSpace, u32, u32)
    {
        ++static_cast<Device *>(opaque)->writes;
        return false;
    }

    Result restore(u32 budget = 64)
    {
        return restoreGraphicsConfiguration(configuration, {this, invoke}, contract, budget);
    }

    Result inspect(Contexts &observed)
    {
        return inspectGraphicsContexts(observed, {this, read, write, nullptr, nullptr}, configuration);
    }
};

void testConfiguration()
{
    auto device = std::make_unique<Device>();
    expect(device->restore() == Result::Ok && device->configuration.restored &&
        device->configuration.calls == 2 && device->configuration.restoreAttempted && !device->violation,
        "exact SMC32 availability and device-18 restore requests succeed in the monitor model");
    expect(device->restore() == Result::NotReady && device->calls.size() == 2,
        "completed or uncertain configuration cannot be retried");
    for (u32 index = 0; index < 5; ++index) {
        Contract contract = Device::contract;
        bool *fields[] = {&contract.platformQualified, &contract.firmwareExited, &contract.exclusiveColdGpu,
            &contract.acpiProfileQualified, &contract.restorePolicySelected};
        *fields[index] = false;
        Configuration configuration = {};
        expect(restoreGraphicsConfiguration(configuration, {device.get(), Device::invoke}, contract, 64) ==
            Result::Unqualified && !configuration.attempted && device->calls.size() == 2,
            "each missing ownership, exit, profile or policy prerequisite prevents every secure call");
    }
    for (u32 budget : {0u, 1u, 65u}) {
        Configuration configuration = {};
        expect(restoreGraphicsConfiguration(configuration, {device.get(), Device::invoke}, Device::contract, budget) ==
            Result::Invalid && !configuration.attempted, "invalid shared call budget is refused before monitor access");
    }
    struct Case { Fault fault; Result expected; u32 calls; };
    const Case cases[] = {
        {Fault::AvailabilityTrap, Result::CallFault, 1},
        {Fault::AvailabilityUnsupported, Result::Unsupported, 1},
        {Fault::AvailabilityZero, Result::Unsupported, 1},
        {Fault::AvailabilityWide, Result::Unsupported, 1},
        {Fault::RestoreTrap, Result::CallFault, 2},
        {Fault::RestoreUnsupported, Result::Unsupported, 2},
        {Fault::RestoreMonitor, Result::MonitorError, 2},
        {Fault::RestoreService, Result::ServiceError, 2},
        {Fault::Busy, Result::MonitorError, 2},
        {Fault::ForeverResume, Result::ResumeLimit, 64},
    };
    for (const Case &test : cases) {
        auto failed = std::make_unique<Device>();
        failed->fault = test.fault;
        expect(failed->restore() == test.expected && !failed->configuration.restored &&
            failed->calls.size() == test.calls && !failed->violation,
            "secure-call failures stop at the actual boundary without ABI fallback or busy retries");
        expect(failed->restore() == Result::NotReady && failed->calls.size() == test.calls,
            "uncertain secure configuration remains pinned and cannot be repeated");
        Contexts observed = {};
        expect(failed->inspect(observed) == Result::Unqualified && !failed->smmuReads && !failed->writes,
            "failed secure setup never opens context-bank MMIO");
    }
    for (Fault fault : {Fault::QueryResume, Fault::RestoreResume}) {
        auto resumed = std::make_unique<Device>();
        resumed->fault = fault;
        expect(resumed->restore() == Result::Ok && resumed->configuration.resumes == 1 &&
            resumed->calls.size() == 3 && !resumed->violation,
            "interrupted standard calls preserve monitor register state including the A6 cookie");
    }
}

void testContexts()
{
    auto device = std::make_unique<Device>();
    Contexts observed = {};
    expect(device->inspect(observed) == Result::Unqualified && !device->smmuReads,
        "unrestored contexts cannot be inspected");
    expect(device->restore() == Result::Ok && device->inspect(observed) == Result::Ok &&
        observed.accessibleDisabled && observed.successfulReads == 18 && observed.reads == 18 &&
        observed.banks[0].validFields == 0x1ff && observed.banks[1].validFields == 0x1ff &&
        !device->violation && !device->writes,
        "read-only context inspection never touches protected global pages or enables DMA");
    for (u32 failedRead = 1; failedRead <= 18; ++failedRead) {
        auto failed = std::make_unique<Device>();
        failed->failedRead = failedRead;
        expect(failed->restore() == Result::Ok && failed->inspect(observed) == Result::IoError &&
            !observed.accessibleDisabled && !observed.lastReadSucceeded &&
            observed.successfulReads == failedRead - 1 && observed.reads == failedRead &&
            failed->smmuReads == failedRead && !failed->writes && !failed->violation,
            "every individual context read fault retains a valid-field mask and stops immediately");
    }
    for (u32 bank = 0; bank < 2; ++bank) {
        auto enabled = std::make_unique<Device>();
        enabled->contexts[bank * 1024] = 1;
        expect(enabled->restore() == Result::Ok && enabled->inspect(observed) == Result::ContextEnabled &&
            !observed.accessibleDisabled && observed.banks[bank].control == 1 && !enabled->writes,
            "inherited enabled translations are observed but never overwritten or claimed reusable");
        enabled->contexts[bank * 1024 + 0x58 / 4] = 2;
        expect(enabled->inspect(observed) == Result::ContextFault && !enabled->writes,
            "context faults take precedence over enabled-state diagnostics and remain uncleared");
    }
    device->gpu[0x22d] = 0;
    const u32 reads = device->smmuReads;
    expect(device->inspect(observed) == Result::NotReady && device->smmuReads == reads,
        "a running command processor prevents context inspection");
}

void checksum(std::vector<u8> &table)
{
    const u32 bytes = static_cast<u32>(table.size());
    for (u32 index = 0; index < 4; ++index) { table[4 + index] = static_cast<u8>(bytes >> (index * 8)); }
    table[9] = 0;
    u8 sum = 0;
    for (u8 byte : table) { sum = static_cast<u8>(sum + byte); }
    table[9] = static_cast<u8>(0u - sum);
}

void testProfile(const char *path)
{
    std::ifstream input(path);
    std::vector<u8> table;
    char first = 0;
    char second = 0;
    while (input >> first >> second) {
        const auto digit = [](char value) -> unsigned { return value >= '0' && value <= '9' ?
            static_cast<unsigned>(value - '0') : static_cast<unsigned>(value - 'a' + 10); };
        table.push_back(static_cast<u8>((digit(first) << 4) | digit(second)));
    }
    expect(table.size() == 280 && isLumiaSmmuProfile(table.data(), static_cast<u32>(table.size())),
        "independently IASL-compiled pinned MMU3 fixture matches the complete expected device declaration");
    if (table.size() != 280) { return; }
    const u32 start = 59;
    expect(table[start] == 0x5b && table[start + 1] == 0x82, "fixture locates the actual MMU3 Device opcode");
    for (u32 index = start; index < table.size(); ++index) {
        std::vector<u8> changed = table;
        changed[index] ^= 1;
        checksum(changed);
        expect(!isLumiaSmmuProfile(changed.data(), static_cast<u32>(changed.size())),
            "every mutated MMU3 byte is rejected even after repairing the ACPI checksum");
    }
    for (u32 bytes = 0; bytes < table.size(); ++bytes) {
        expect(!isLumiaSmmuProfile(table.data(), bytes), "every truncated ACPI length is refused");
    }
    std::vector<u8> duplicate = table;
    duplicate.insert(duplicate.end(), table.begin() + start, table.end());
    checksum(duplicate);
    expect(!isLumiaSmmuProfile(duplicate.data(), static_cast<u32>(duplicate.size())),
        "duplicate MMU3 signatures cannot qualify a secure restore");
    table[9] ^= 1;
    expect(!isLumiaSmmuProfile(table.data(), static_cast<u32>(table.size())), "bad ACPI checksum prevents secure setup");
}

}

int main(int argc, char **argv)
{
    if (argc != 2) { return 2; }
    testConfiguration();
    testContexts();
    testProfile(argv[1]);
    std::printf("secure GPU SMMU model: %u checks, %u failures (not handset execution)\n", checks, failures);
    return failures ? 1 : 0;
}
