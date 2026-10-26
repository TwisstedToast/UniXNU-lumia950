#include "FramebufferCore.h"

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
    uint32_t storage[2 + 12 * 12];
    struct ProbeFramebuffer framebuffer = {
        (uintptr_t)&storage[1], 12 * 12 * 4, 10, 12, 12, 1
    };
    struct ProbeFramebuffer changed;
    uint32_t copy[2 + 12 * 12];
    for (size_t index = 0; index < sizeof(storage) / sizeof(storage[0]); index++) {
        storage[index] = 0xdeadbeef;
    }
    CHECK(ProbeFramebufferValid(&framebuffer));
    CHECK(!ProbeFramebufferValid(NULL));
    changed = framebuffer;
    changed.base = 0;
    CHECK(!ProbeFramebufferValid(&changed));
    changed = framebuffer;
    changed.base++;
    CHECK(!ProbeFramebufferValid(&changed));
    changed = framebuffer;
    changed.stride = 9;
    CHECK(!ProbeFramebufferValid(&changed));
    changed = framebuffer;
    changed.byte_size--;
    CHECK(!ProbeFramebufferValid(&changed));
    changed = framebuffer;
    changed.pixel_format = 3;
    CHECK(!ProbeFramebufferValid(&changed));
    changed = framebuffer;
    changed.height = 0;
    CHECK(!ProbeFramebufferValid(&changed));
    changed = framebuffer;
    changed.base = UINT64_MAX - 3;
    CHECK(!ProbeFramebufferValid(&changed));
    CHECK(ProbeFramebufferColor(0, 1, 2, 3) == 0x030201);
    CHECK(ProbeFramebufferColor(1, 1, 2, 3) == 0x010203);
    CHECK(ProbeFramebufferColor(3, 1, 2, 3) == 0);
    CHECK(ProbeRangesOverlap(0x400000, 0x800000, 0x500000, 4096));
    CHECK(!ProbeRangesOverlap(0x400000, 0x800000, 0xc00000, 4096));
    CHECK(!ProbeRangesOverlap(1, 0, 1, 1));
    CHECK(ProbeRangesOverlap(UINT64_MAX, 4096, 0, 4096));
    ProbeFillRectangle(&framebuffer, 0, 0, 10, 12, 0);
    CHECK(storage[0] == 0xdeadbeef && storage[145] == 0xdeadbeef);
    for (size_t row = 0; row < 12; row++) {
        CHECK(storage[1 + row * 12 + 10] == 0xdeadbeef);
        CHECK(storage[1 + row * 12 + 11] == 0xdeadbeef);
    }
    ProbeFillRectangle(&framebuffer, 8, 10, UINT32_MAX, UINT32_MAX, 0x00ff00);
    CHECK(storage[1 + 10 * 12 + 8] == 0x00ff00);
    CHECK(storage[1 + 11 * 12 + 9] == 0x00ff00);
    CHECK(storage[1 + 9 * 12 + 8] == 0);
    memcpy(copy, storage, sizeof(storage));
    ProbeFillRectangle(&framebuffer, UINT32_MAX, UINT32_MAX, 1, 1, 7);
    CHECK(memcmp(copy, storage, sizeof(storage)) == 0);
    ProbeDrawText(&framebuffer, 0, 0, 1, "A", 0x00ff00);
    CHECK(storage[1 + 1] == 0x00ff00);
    CHECK(storage[1] == 0);
    CHECK(storage[1 + 3 * 12 + 2] == 0x00ff00);
    memcpy(copy, storage, sizeof(storage));
    ProbeDrawText(&framebuffer, 0, 0, 0, "A", 1);
    ProbeDrawText(&framebuffer, 0, 0, 17, "A", 1);
    ProbeDrawText(&framebuffer, 0, 9, 1, "A", 1);
    CHECK(memcmp(copy, storage, sizeof(storage)) == 0);
    ProbeDrawText(&framebuffer, 9, 0, 1, "A", 0x123456);
    CHECK(storage[0] == 0xdeadbeef && storage[145] == 0xdeadbeef);
    for (size_t row = 0; row < 12; row++) {
        CHECK(storage[1 + row * 12 + 10] == 0xdeadbeef);
        CHECK(storage[1 + row * 12 + 11] == 0xdeadbeef);
    }
    printf("Framebuffer core: %u checks passed\n", check_count);
    return 0;
}
