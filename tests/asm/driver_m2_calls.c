/*
 * Author: dhjs0000
 *
 * M2 calls: recursion, stack arguments in both directions, indirect calls,
 * and a direct call to the CRT's putchar.
 */
#include <stdio.h>

extern int fib_rec(int n);
extern int sum5(int a, int b, int c, int d, int e);
extern int sum6(int a, int b, int c, int d, int e, int f);
extern int call_sum6(void);
extern int call_sum5(void);
extern int call_sum5_2(int x);
extern int forward5(int a, int b, int c, int d, int e);
extern int add2(int x);
extern int apply(void *fp, int x);
extern int emit_A(void);
extern long long call_add64(void);
extern signed char id8(signed char x);
extern int call_id8(int x);

int main(void) {
    struct Check {
        const char *name;
        int got;
        int want;
    } checks[] = {
        {"fib_rec(10)", fib_rec(10), 55},
        {"fib_rec(20)", fib_rec(20), 6765},
        {"sum5(1..5)", sum5(1, 2, 3, 4, 5), 15},
        {"sum6(1..6)", sum6(1, 2, 3, 4, 5, 6), 21},
        {"call_sum6()", call_sum6(), 21},
        {"call_sum5()", call_sum5(), 15},
        {"call_sum5_2(1)", call_sum5_2(1), 15},
        {"forward5(1..5)", forward5(1, 2, 3, 4, 5), 15},
        {"apply(add2,40)", apply((void *)add2, 40), 42},
        {"call_add64()", (int)call_add64(), 5},
        {"id8(-3)", id8(-3), -3},
        {"call_id8(200)", call_id8(200), -56},
    };
    int failures = 0;
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        printf("%s = %d (want %d)\n", checks[i].name, checks[i].got,
               checks[i].want);
        if (checks[i].got != checks[i].want) {
            failures += 1;
        }
    }
    int emitted = emit_A();
    printf("\nemit_A = %d (want 65)\n", emitted);
    if (emitted != 65) {
        failures += 1;
    }
    return failures == 0 ? 0 : 1;
}
