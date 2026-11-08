/*
 * Author: dhjs0000
 *
 * M1 driver for conditional branches and the temporary switch compare chain.
 */
#include <stdio.h>

extern int classify(int n);

int main(void) {
    struct Check {
        int input;
        int want;
    } checks[] = {
        {0, 0},
        {-5, 5},
        {1, 20},
        {2, 30},
        {7, 30},
    };
    int failures = 0;
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        int got = classify(checks[i].input);
        printf("classify(%d)=%d (want %d)\n", checks[i].input, got,
               checks[i].want);
        if (got != checks[i].want) {
            failures += 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
