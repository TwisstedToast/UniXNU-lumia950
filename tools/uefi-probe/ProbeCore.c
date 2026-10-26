#include "ProbeCore.h"

int ProbeAddMemoryRange(struct ProbeMemorySummary *summary,
                        enum ProbeMemoryKind kind, uint64_t physical_start,
                        uint64_t page_count)
{
    uint64_t byte_count;

    if (summary == NULL || kind < 0 || kind >= PROBE_MEMORY_KIND_COUNT ||
        (physical_start & 4095) != 0 || page_count == 0 ||
        page_count > UINT64_MAX / 4096 ||
        summary->descriptor_count == SIZE_MAX) {
        return 0;
    }
    byte_count = page_count * 4096;
    if (physical_start > UINT64_MAX - byte_count ||
        summary->bytes_by_kind[kind] > UINT64_MAX - byte_count) {
        return 0;
    }
    summary->bytes_by_kind[kind] += byte_count;
    summary->descriptor_count++;
    if (kind == PROBE_MEMORY_FREE && byte_count > summary->largest_free_size) {
        summary->largest_free_base = physical_start;
        summary->largest_free_size = byte_count;
    }
    return 1;
}

int ProbeFormatUnsigned(uint64_t value, unsigned int radix,
                        unsigned int minimum_digits, char *output,
                        size_t capacity)
{
    static const char alphabet[] = "0123456789abcdef";
    char reversed[64];
    size_t digit_count = 0;
    size_t output_count;

    if (output == NULL || capacity == 0) {
        return 0;
    }
    output[0] = '\0';
    if ((radix != 10 && radix != 16) || minimum_digits > sizeof(reversed)) {
        return 0;
    }
    do {
        reversed[digit_count++] = alphabet[value % radix];
        value /= radix;
    } while (value != 0);
    output_count = digit_count > minimum_digits ? digit_count : minimum_digits;
    if (output_count >= capacity) {
        return 0;
    }
    for (size_t position = 0; position < output_count; position++) {
        output[position] = position < output_count - digit_count
            ? '0' : reversed[output_count - position - 1];
    }
    output[output_count] = '\0';
    return 1;
}
