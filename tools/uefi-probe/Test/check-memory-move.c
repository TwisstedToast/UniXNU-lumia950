#include <stddef.h>
#include <stdio.h>

void *ProbeMemoryMove(void *destination, const void *source, size_t bytes);

int main(void)
{
    unsigned checks = 0;
    unsigned failures = 0;
    for (size_t source = 0; source < 32; ++source) {
        for (size_t destination = 0; destination < 32; ++destination) {
            for (size_t count = 0; count <= 64; ++count) {
                unsigned char data[96];
                unsigned char expected[96];
                unsigned char snapshot[64];
                for (size_t index = 0; index < sizeof(data); ++index) {
                    data[index] = expected[index] = (unsigned char)(index ^ 0xa7);
                }
                for (size_t index = 0; index < count; ++index) { snapshot[index] = data[source + index]; }
                for (size_t index = 0; index < count; ++index) { expected[destination + index] = snapshot[index]; }
                ++checks;
                if (ProbeMemoryMove(data + destination, data + source, count) != data + destination) { ++failures; }
                for (size_t index = 0; index < sizeof(data); ++index) {
                    ++checks;
                    if (data[index] != expected[index]) { ++failures; }
                }
            }
        }
    }
    ++checks;
    if (ProbeMemoryMove(NULL, NULL, 0) != NULL) { ++failures; }
    printf("Freestanding overlapping copy: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
