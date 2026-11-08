/*
 * Author: dhjs0000
 *
 * add_mul: the design document's first M1 acceptance example.
 * Expected: add_mul(6, 7) = 6*4 + 7 = 31.
 */
#include <stdio.h>

extern int add_mul(int a, int b);

int main(void) {
    int value = add_mul(6, 7);
    printf("add_mul(6, 7) = %d\n", value);
    return value == 31 ? 0 : 1;
}
