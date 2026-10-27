#include <stddef.h>
#include <stdint.h>

void *memmove(void *destination, const void *source, size_t bytes)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    if ((uintptr_t)output < (uintptr_t)input) {
        for (size_t index = 0; index < bytes; ++index) { output[index] = input[index]; }
    } else if (output != input) {
        while (bytes) { --bytes; output[bytes] = input[bytes]; }
    }
    return destination;
}
