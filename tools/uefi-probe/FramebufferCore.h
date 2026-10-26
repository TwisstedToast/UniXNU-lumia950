#ifndef LUMIA_FRAMEBUFFER_CORE_H
#define LUMIA_FRAMEBUFFER_CORE_H

#include <stdint.h>

struct ProbeFramebuffer {
    uint64_t base;
    uint64_t byte_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
};

int ProbeFramebufferValid(const struct ProbeFramebuffer *framebuffer);
int ProbeRangesOverlap(uint64_t first_base, uint64_t first_size,
                       uint64_t second_base, uint64_t second_size);
uint32_t ProbeFramebufferColor(uint32_t pixel_format,
                               uint8_t red, uint8_t green, uint8_t blue);
void ProbeFillRectangle(const struct ProbeFramebuffer *framebuffer,
                         uint32_t left, uint32_t top, uint32_t width,
                         uint32_t height, uint32_t color);
void ProbeDrawText(const struct ProbeFramebuffer *framebuffer,
                    uint32_t left, uint32_t top, uint32_t scale,
                    const char *text, uint32_t color);

#endif
