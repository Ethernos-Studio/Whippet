/*
 * Author: dhjs0000
 *
 * M1 acceptance driver for the design's add_mul example.
 */
#include <stdio.h>

extern int add_mul(int a, int b);

int main(void) {
    int value = add_mul(6, 7);
    printf("add_mul(6,7)=%d\n", value);
    return value == 31 ? 0 : 1;
}
