#!/usr/bin/env python3
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "pe_identify_machine.c": "3557bb1b2c17f721f5cad529299aee49d61e2000fb0768ee6c22791340343ccd",
    "machine_routines.c": "663b25f0d52c811dc09a646ab8a5c904826f4312a1bfd23c2d3a9744cb3eb2a1",
}

PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef unsigned DTEntry;
typedef unsigned OpaqueDTEntryIterator;
typedef void boot_args;
#define kSuccess 0
#define USEC_PER_SEC 1000000
static unsigned soc_calls;
static unsigned frequency_reads;
static unsigned arm64_eventi;
static uintptr_t soc_base;
static unsigned bus_known;
static uint32_t timebase = 19200000;
static uint32_t bus_rate = 100000000;
static uint64_t cpu_rate = 1000000000;
static uintptr_t pe_arm_get_soc_base_phys(void) { ++soc_calls; return soc_base; }
static int SecureDTLookupEntry(void *parent, const char *name, DTEntry *entry) {
    (void)parent;
    assert(!strcmp(name, "/cpus"));
    *entry = 1;
    return kSuccess;
}
static int SecureDTInitEntryIterator(DTEntry parent, OpaqueDTEntryIterator *iterator) {
    assert(parent == 1);
    *iterator = 0;
    return kSuccess;
}
static int SecureDTIterateEntries(OpaqueDTEntryIterator *iterator, DTEntry *entry) {
    if (*iterator == 2) { return -1; }
    *entry = ++*iterator;
    return kSuccess;
}
static int SecureDTGetProperty(DTEntry entry, const char *name, const void **value, unsigned *size) {
    assert(entry == 1 || entry == 2);
    if (!strcmp(name, "state")) {
        *value = entry == 1 ? "running" : "waiting";
        *size = 8;
    } else if (!strcmp(name, "timebase-frequency")) {
        assert(entry == 1);
        *value = &timebase;
        *size = sizeof(timebase);
        ++frequency_reads;
    } else if (bus_known && !strcmp(name, "bus-frequency")) {
        *value = &bus_rate;
        *size = sizeof(bus_rate);
    } else if (bus_known && !strcmp(name, "clock-frequency")) {
        *value = &cpu_rate;
        *size = sizeof(cpu_rate);
    } else { return -1; }
    return kSuccess;
}
static int PE_parse_boot_argn(const char *name, void *value, size_t size) {
    (void)value;
    assert(!strcmp(name, "wfe_events_sec") && size == 8);
    return 0;
}
static int test_flsll(uint64_t value) { return value ? 64 - __builtin_clzll(value) : 0; }
#define flsll test_flsll
'''

SUFFIX = r'''
int main(int argc, char **argv) {
    assert(argc == 3);
    soc_base = strtoul(argv[1], NULL, 0);
    bus_known = (unsigned)strtoul(argv[2], NULL, 0);
    pe_identify_machine(NULL);
    if ((!XNU_NATIVE_ARCH_TIMER || !PATCHED) && !soc_base) {
        assert(!gPEClockFrequencyInfo.timebase_frequency_hz);
        assert(!frequency_reads && soc_calls == 1);
        puts("Legacy zero-base clock discovery stops before initialization");
        return 0;
    }
    assert(gPEClockFrequencyInfo.timebase_frequency_hz == 19200000);
    assert(gPEClockFrequencyInfo.dec_clock_rate_hz == 19200000);
    assert(frequency_reads == 1);
    assert(soc_calls == (XNU_NATIVE_ARCH_TIMER && PATCHED ? 0 : 1));
    assert(gPEClockFrequencyInfo.bus_clock_rate_hz == (bus_known ? 100000000 : 0));
    assert(gPEClockFrequencyInfo.cpu_clock_rate_hz == (bus_known ? 1000000000 : 0));
    assert(gPEClockFrequencyInfo.bus_to_cpu_rate_num == (bus_known ? 20 : 0));
    assert(gPEClockFrequencyInfo.bus_to_dec_rate_den == (bus_known ? 5 : 0));
    wfe_timeout_configure();
    assert(arm64_eventi == 4);
    puts("Real XNU clock discovery and WFE configuration pass with UBSan enabled");
    return 0;
}
'''


def function(source, name):
    match = re.search(r"\n(?:void\n)?" + name + r"\([^;]*?\)\n\{", source)
    if not match:
        raise ValueError(f"missing actual source function {name}")
    stop = source.index("\n}", match.end()) + 2
    return source[match.start():stop]


def main():
    parser = argparse.ArgumentParser(description="Compile the pinned real XNU clock/WFE functions with a mocked DT, not hardware")
    parser.add_argument("--identify", required=True, type=Path)
    parser.add_argument("--routines", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    original = args.identify.read_bytes()
    routines = args.routines.read_bytes()
    for name, blob in [("pe_identify_machine.c", original), ("machine_routines.c", routines)]:
        if hashlib.sha256(blob).hexdigest() != SOURCES[name]:
            raise ValueError(f"unqualified Apple source {name}")
    delta = (ROOT / "ci/unixnu-16x.patch").read_text().split(
        "diff --git a/patches/xnu-8796/native-platform-clocks.patch b/patches/xnu-8796/native-platform-clocks.patch\n", 1)[1]
    delta = delta.split("\ndiff --git ", 1)[0].splitlines()[4:]
    patch = "\n".join(line[1:] for line in delta) + "\n"
    with tempfile.TemporaryDirectory(prefix="lumia-native-clocks-") as temporary:
        directory = Path(temporary)
        source = directory / "pexpert/arm/pe_identify_machine.c"
        source.parent.mkdir(parents=True)
        source.write_bytes(original)
        subprocess.run(["git", "apply", "--whitespace=nowarn", "-"], input=patch, text=True, cwd=directory, check=True)
        patched = source.read_text()
        identify = function(patched, "pe_identify_machine")
        fields = sorted(set(re.findall(r"gPEClockFrequencyInfo\.([a-z_]+)", identify)))
        structure = "typedef struct {\n" + "\n".join(
            f"{'uint64_t' if 'frequency' in field else 'uint32_t'} {field};" for field in fields)
        structure += "\n} clock_frequency_info_t;\nstatic clock_frequency_info_t gPEClockFrequencyInfo;\n"
        for label, native, changed in [("original", 1, False), ("legacy", 0, True), ("native", 1, True)]:
            fragment = function(patched if changed else original.decode(), "pe_identify_machine")
            program = directory / f"{label}.c"
            program.write_text(PREFIX + structure + fragment + "\n" + function(routines.decode(), "wfe_timeout_configure") + SUFFIX)
            executable = directory / label
            subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-fsanitize=undefined", "-fno-sanitize-recover=all", "-D_DEFAULT_SOURCE",
                f"-DXNU_NATIVE_ARCH_TIMER={native}", f"-DPATCHED={int(changed)}", str(program), "-o", str(executable)], check=True)
            cases = [(0, 0)] if not changed else ([(0, 0), (1, 1)] if not native else [(0, 0), (1, 0), (0, 1)])
            for base, known in cases:
                subprocess.run([str(executable), str(base), str(known)], check=True)
    if args.identify.read_bytes() != original:
        raise ValueError("upstream source mutated")
    print("PASS: original failure reproduced; 32/64-bit DT values, unknown clocks, native zero-base and legacy paths checked")


if __name__ == "__main__":
    main()
