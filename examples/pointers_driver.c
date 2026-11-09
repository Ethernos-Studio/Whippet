/*
 * Author: dhjs0000
 *
 * Alloca/load/store and passing a pointer to an alloca across a call.
 */
#include <stdio.h>

extern int use_ptr(void);
extern int memory(int x);

int main(void) {
    int a = use_ptr();
    int b = memory(21);
    printf("use_ptr() = %d\n", a);
    printf("memory(21) = %d\n", b);
    return (a == 42 && b == 42) ? 0 : 1;
}
