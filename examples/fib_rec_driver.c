/*
 * Author: dhjs0000
 *
 * Recursive fib: the design document's M2 acceptance example.
 */
#include <stdio.h>

extern int fib_rec(int n);

int main(void) {
    struct Check {
        int n;
        int want;
    } checks[] = {
        {0, 0},
        {1, 1},
        {2, 1},
        {10, 55},
        {20, 6765},
    };
    int failures = 0;
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        int got = fib_rec(checks[i].n);
        printf("fib_rec(%d) = %d\n", checks[i].n, got);
        if (got != checks[i].want) {
            failures += 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
