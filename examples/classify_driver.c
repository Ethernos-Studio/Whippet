/*
 * Author: dhjs0000
 *
 * Branches and switch. The switch is positional: case index 0 -> case0,
 * index 1 -> case1, everything else -> other.
 */
#include <stdio.h>

extern int classify(int n);

int main(void) {
    struct Check {
        int n;
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
        int got = classify(checks[i].n);
        printf("classify(%d) = %d\n", checks[i].n, got);
        if (got != checks[i].want) {
            failures += 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
