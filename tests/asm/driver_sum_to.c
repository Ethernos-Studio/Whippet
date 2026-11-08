/*
 * Author: dhjs0000
 *
 * M1 acceptance driver for a phi/loop function: sum_to(10) = 0+1+...+10.
 */
#include <stdio.h>

extern int sum_to(int n);

int main(void) {
    int value = sum_to(10);
    printf("sum_to(10)=%d\n", value);
    return value == 55 ? 0 : 1;
}
