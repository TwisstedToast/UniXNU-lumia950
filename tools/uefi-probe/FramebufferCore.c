#include "FramebufferCore.h"

#include <stddef.h>

static const uint8_t letter_rows[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
};

static const uint8_t digit_rows[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
};

int ProbeFramebufferValid(const struct ProbeFramebuffer *framebuffer)
{
    uint64_t row_bytes;
    uint64_t required_size;
    if (framebuffer == NULL || framebuffer->base == 0 ||
        (framebuffer->base & 3) != 0 || framebuffer->width == 0 ||
        framebuffer->height == 0 || framebuffer->stride < framebuffer->width ||
        framebuffer->pixel_format > 1) {
        return 0;
    }
    row_bytes = (uint64_t)framebuffer->stride * 4;
    if (framebuffer->height > UINT64_MAX / row_bytes) {
        return 0;
    }
    required_size = row_bytes * framebuffer->height;
    return required_size <= framebuffer->byte_size &&
        framebuffer->base <= UINT64_MAX - required_size;
}

int ProbeRangesOverlap(uint64_t first_base, uint64_t first_size,
                       uint64_t second_base, uint64_t second_size)
{
    if (first_size == 0 || second_size == 0) {
        return 0;
    }
    if (first_base > UINT64_MAX - first_size || second_base > UINT64_MAX - second_size) {
        return 1;
    }
    return first_base < second_base + second_size && second_base < first_base + first_size;
}

uint32_t ProbeFramebufferColor(uint32_t pixel_format,
                               uint8_t red, uint8_t green, uint8_t blue)
{
    if (pixel_format == 0) {
        return red | (uint32_t)green << 8 | (uint32_t)blue << 16;
    }
    if (pixel_format == 1) {
        return blue | (uint32_t)green << 8 | (uint32_t)red << 16;
    }
    return 0;
}

void ProbeFillRectangle(const struct ProbeFramebuffer *framebuffer,
                         uint32_t left, uint32_t top, uint32_t width,
                         uint32_t height, uint32_t color)
{
    volatile uint32_t *pixels;
    if (!ProbeFramebufferValid(framebuffer) ||
        left >= framebuffer->width || top >= framebuffer->height) {
        return;
    }
    if (width > framebuffer->width - left) {
        width = framebuffer->width - left;
    }
    if (height > framebuffer->height - top) {
        height = framebuffer->height - top;
    }
    pixels = (volatile uint32_t *)(uintptr_t)framebuffer->base;
    for (uint32_t row = 0; row < height; row++) {
        uint64_t offset = (uint64_t)(top + row) * framebuffer->stride + left;
        for (uint32_t column = 0; column < width; column++) {
            pixels[offset + column] = color;
        }
    }
}

void ProbeDrawText(const struct ProbeFramebuffer *framebuffer,
                    uint32_t left, uint32_t top, uint32_t scale,
                    const char *text, uint32_t color)
{
    uint32_t cursor = left;
    if (!ProbeFramebufferValid(framebuffer) || text == NULL || scale == 0 || scale > 16) {
        return;
    }
    while (*text != '\0') {
        unsigned char character = (unsigned char)*text++;
        const uint8_t *rows = NULL;
        if (cursor >= framebuffer->width || top >= framebuffer->height ||
            7 * scale > framebuffer->height - top) {
            return;
        }
        if (character >= 'a' && character <= 'z') {
            character -= 'a' - 'A';
        }
        if (character >= 'A' && character <= 'Z') {
            rows = letter_rows[character - 'A'];
        } else if (character >= '0' && character <= '9') {
            rows = digit_rows[character - '0'];
        }
        if (rows != NULL) {
            for (uint32_t row = 0; row < 7; row++) {
                for (uint32_t column = 0; column < 5; column++) {
                    if ((rows[row] & (1u << (4 - column))) != 0 &&
                        column * scale <= framebuffer->width - cursor) {
                        ProbeFillRectangle(framebuffer, cursor + column * scale,
                                             top + row * scale, scale, scale, color);
                    }
                }
            }
        }
        if (6 * scale > framebuffer->width - cursor) {
            return;
        }
        cursor += 6 * scale;
    }
}
