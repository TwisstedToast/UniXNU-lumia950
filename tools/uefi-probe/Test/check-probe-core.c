#include "ProbeCore.h"

#include <stdio.h>
#include <string.h>

static unsigned int check_count;

#define CHECK(condition) do { \
    check_count++; \
    if (!(condition)) { \
        fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    struct ProbeMemorySummary summary = {0};
    struct ProbeMemorySummary saved;
    char buffer[65];

    CHECK(ProbeAddMemoryRange(&summary, PROBE_MEMORY_FREE, 0x80000000, 256));
    CHECK(summary.bytes_by_kind[PROBE_MEMORY_FREE] == 1024 * 1024);
    CHECK(summary.largest_free_base == 0x80000000);
    CHECK(summary.largest_free_size == 1024 * 1024);
    CHECK(ProbeAddMemoryRange(&summary, PROBE_MEMORY_RECLAIMABLE, 0x81000000, 128));
    CHECK(summary.bytes_by_kind[PROBE_MEMORY_RECLAIMABLE] == 512 * 1024);
    CHECK(ProbeAddMemoryRange(&summary, PROBE_MEMORY_RUNTIME, 0x82000000, 1));
    CHECK(summary.bytes_by_kind[PROBE_MEMORY_RUNTIME] == 4096);
    CHECK(ProbeAddMemoryRange(&summary, PROBE_MEMORY_OTHER, 0xf9000000, 16));
    CHECK(summary.bytes_by_kind[PROBE_MEMORY_OTHER] == 65536);
    CHECK(summary.largest_free_base == 0x80000000);
    CHECK(ProbeAddMemoryRange(&summary, PROBE_MEMORY_FREE, 0x90000000, 512));
    CHECK(summary.largest_free_base == 0x90000000);
    CHECK(summary.largest_free_size == 2 * 1024 * 1024);
    CHECK(summary.descriptor_count == 5);

    saved = summary;
    CHECK(!ProbeAddMemoryRange(&summary, PROBE_MEMORY_FREE, 1, 1));
    CHECK(!ProbeAddMemoryRange(&summary, PROBE_MEMORY_FREE, 0, 0));
    CHECK(!ProbeAddMemoryRange(&summary, PROBE_MEMORY_FREE, 0, UINT64_MAX));
    CHECK(!ProbeAddMemoryRange(&summary, PROBE_MEMORY_FREE, UINT64_MAX - 4095, 1));
    CHECK(!ProbeAddMemoryRange(&summary, PROBE_MEMORY_KIND_COUNT, 0, 1));
    CHECK(!ProbeAddMemoryRange(&summary, (enum ProbeMemoryKind)-1, 0, 1));
    CHECK(!ProbeAddMemoryRange(NULL, PROBE_MEMORY_FREE, 0, 1));
    CHECK(memcmp(&summary, &saved, sizeof(summary)) == 0);
    summary.bytes_by_kind[PROBE_MEMORY_FREE] = UINT64_MAX;
    CHECK(!ProbeAddMemoryRange(&summary, PROBE_MEMORY_FREE, 0, 1));
    summary = saved;
    summary.descriptor_count = SIZE_MAX;
    CHECK(!ProbeAddMemoryRange(&summary, PROBE_MEMORY_FREE, 0, 1));

    CHECK(ProbeFormatUnsigned(0, 10, 1, buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "0") == 0);
    CHECK(ProbeFormatUnsigned(UINT64_MAX, 10, 1, buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "18446744073709551615") == 0);
    CHECK(ProbeFormatUnsigned(UINT64_MAX, 16, 16, buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "ffffffffffffffff") == 0);
    CHECK(ProbeFormatUnsigned(0xf9000000, 16, 16, buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "00000000f9000000") == 0);
    CHECK(ProbeFormatUnsigned(42, 10, 5, buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "00042") == 0);
    CHECK(ProbeFormatUnsigned(1, 10, 64, buffer, sizeof(buffer)));
    CHECK(strlen(buffer) == 64 && buffer[63] == '1');
    CHECK(!ProbeFormatUnsigned(1, 10, 65, buffer, sizeof(buffer)));
    CHECK(buffer[0] == '\0');
    CHECK(!ProbeFormatUnsigned(1, 2, 1, buffer, sizeof(buffer)));
    CHECK(!ProbeFormatUnsigned(10, 10, 1, buffer, 2));
    CHECK(buffer[0] == '\0');
    CHECK(ProbeFormatUnsigned(0, 10, 0, buffer, 2));
    CHECK(strcmp(buffer, "0") == 0);
    CHECK(!ProbeFormatUnsigned(0, 10, 0, buffer, 0));
    CHECK(!ProbeFormatUnsigned(0, 10, 0, NULL, 2));
    printf("UEFI probe core: %u checks passed\n", check_count);
    return 0;
}
