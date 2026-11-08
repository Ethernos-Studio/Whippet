/*
 * Author: dhjs0000
 *
 * Arithmetic, shifts, compare and select.
 */
#include <stdio.h>

extern int mix(int x, int y);

int main(void) {
    struct Check {
        int x;
        int y;
        int want;
    } checks[] = {
        {7, 9, 100},
        {200, 100, 144},
        {-1, 1, 100},
    };
    int failures = 0;
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        int got = mix(checks[i].x, checks[i].y);
        printf("mix(%d, %d) = %d\n", checks[i].x, checks[i].y, got);
        if (got != checks[i].want) {
            failures += 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
