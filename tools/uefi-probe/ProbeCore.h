#ifndef LUMIA_PROBE_CORE_H
#define LUMIA_PROBE_CORE_H

#include <stddef.h>
#include <stdint.h>

enum ProbeMemoryKind {
    PROBE_MEMORY_FREE,
    PROBE_MEMORY_RECLAIMABLE,
    PROBE_MEMORY_RUNTIME,
    PROBE_MEMORY_OTHER,
    PROBE_MEMORY_KIND_COUNT
};

struct ProbeMemorySummary {
    uint64_t bytes_by_kind[PROBE_MEMORY_KIND_COUNT];
    uint64_t largest_free_base;
    uint64_t largest_free_size;
    size_t descriptor_count;
};

int ProbeAddMemoryRange(struct ProbeMemorySummary *summary,
                        enum ProbeMemoryKind kind, uint64_t physical_start,
                        uint64_t page_count);
int ProbeFormatUnsigned(uint64_t value, unsigned int radix,
                        unsigned int minimum_digits, char *output,
                        size_t capacity);

#endif
