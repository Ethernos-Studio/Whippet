/*
 * Author: dhjs0000
 *
 * M1 integer-subset driver: arithmetic, signed division/remainder, shifts,
 * comparisons, select, casts, branches, spills and callee-saved registers.
 */
#include <stdio.h>

extern int calc(int x);
extern int branch(int x);
extern int small(signed char a, short b);
extern int spill(int x);
extern int small_ops(signed char a, short b);
extern int unsigned_ops(int a, int b);
extern int signed_ops(int a, int b);
extern int swap(int n);
extern long long wide_ops(long long a, long long b);
extern int four(int a, int b, int c, int d);

int main(void) {
    struct Check {
        const char *name;
        int got;
        int want;
    } checks[] = {
        {"calc(5)", calc(5), -1},
        {"branch(5)", branch(5), 1},
        {"branch(-3)", branch(-3), -1},
        {"small(-5,3)", small(-5, 3), 2},
        {"spill(7)", spill(7), 350},
        {"small_ops(-3,5)", small_ops(-3, 5), 11},
        {"small_ops(3,5)", small_ops(3, 5), 255},
        {"unsigned_ops(100,7)", unsigned_ops(100, 7), 14},
        {"signed_ops(-100,7)", signed_ops(-100, 7), -100},
        {"swap(0)", swap(0), 2},
        {"swap(1)", swap(1), 1},
        {"wide_ops(100,23)", (int)wide_ops(100, 23), 1},
        {"wide_ops(-100,23)", (int)wide_ops(-100, 23), -1},
        {"four(1,2,3,4)", four(1, 2, 3, 4), 10},
    };
    int failures = 0;
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        printf("%s = %d (want %d)\n", checks[i].name, checks[i].got,
               checks[i].want);
        if (checks[i].got != checks[i].want) {
            failures += 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
