/*
 * Author: dhjs0000
 *
 * M2 memory: alloca, load/store, pointer arithmetic, pointer arguments and
 * i8/i16/i32/i64 memory widths.
 */
#include <stdio.h>
#include <stdint.h>

extern int use_ptr(void);
extern int memory(int x);
extern int widths(void *p);
extern int64_t wide(void *p);

int main(void) {
    int pointer_value = use_ptr();
    int memory_value = memory(21);
    unsigned char buffer[16] = {0};
    int width_value = widths(buffer);
    int64_t wide_value = wide(buffer);

    printf("use_ptr() = %d (want 42)\n", pointer_value);
    printf("memory(21) = %d (want 42)\n", memory_value);
    printf("widths(buffer) = %d (want 65790)\n", width_value);
    printf("wide(buffer) = %lld (want 1234605616436508552)\n",
           (long long)wide_value);

    int failures = 0;
    failures += pointer_value != 42;
    failures += memory_value != 42;
    failures += width_value != 65790;
    failures += wide_value != INT64_C(0x1122334455667788);
    return failures == 0 ? 0 : 1;
}
