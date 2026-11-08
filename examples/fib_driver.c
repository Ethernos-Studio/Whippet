/*
 * Author: dhjs0000
 *
 * Iterative fib with phi/loop; checks base cases and fib(10) = 55.
 */
#include <stdio.h>

extern int fib(int n);

int main(void) {
    struct Check {
        int n;
        int want;
    } checks[] = {
        {0, 0},
        {1, 1},
        {2, 1},
        {3, 2},
        {10, 55},
    };
    int failures = 0;
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        int got = fib(checks[i].n);
        printf("fib(%d) = %d\n", checks[i].n, got);
        if (got != checks[i].want) {
            failures += 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
