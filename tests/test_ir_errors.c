/*
 * M0 acceptance: malformed input must be rejected with a useful location.
 * Each case names the expected line/column of the diagnostic; a zero means
 * "do not check this part" (used where the exact column is less interesting).
 */
#include "whippet/ir.h"

#include <stdio.h>
#include <string.h>

typedef struct ErrorCase {
    const char *name;
    const char *source;
    unsigned line;
    unsigned column;
} ErrorCase;

static const ErrorCase kCases[] = {
    {
        "unknown opcode",
        "func @f(i32 %a) -> i32 {\n"
        "entry:\n"
        "  %v0 = frobnicate i32 %a\n"
        "  ret i32 %a\n"
        "}\n",
        3,
        9,
    },
    {
        "unknown type",
        "func @f(i32 %a) -> i32 {\n"
        "entry:\n"
        "  %v0 = add i33 %a, 1\n"
        "  ret i32 %a\n"
        "}\n",
        3,
        13,
    },
    {
        "undefined value",
        "func @f(i32 %a) -> i32 {\n"
        "entry:\n"
        "  %v0 = add i32 %a, %undefined\n"
        "  ret i32 %a\n"
        "}\n",
        3,
        21,
    },
    {
        "duplicate value",
        "func @f(i32 %a) -> i32 {\n"
        "entry:\n"
        "  %v0 = add i32 %a, 1\n"
        "  %v0 = add i32 %v0, 2\n"
        "  ret i32 %v0\n"
        "}\n",
        4,
        3,
    },
    {
        "missing block terminator",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  %v0 = add i32 1, 2\n"
        "}\n",
        4,
        1,
    },
    {
        "instruction after terminator",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  ret i32 0\n"
        "  %v0 = add i32 1, 2\n"
        "}\n",
        4,
        3,
    },
    {
        "unknown comparison predicate",
        "func @f(i32 %a) -> i32 {\n"
        "entry:\n"
        "  %v0 = icmp bogus i32 %a, 1\n"
        "  ret i32 %v0\n"
        "}\n",
        3,
        14,
    },
    {
        "undefined block",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  br nowhere\n"
        "}\n",
        3,
        6,
    },
    {
        "phi after non-phi",
        "func @f(i32 %a) -> i32 {\n"
        "entry:\n"
        "  %v0 = add i32 %a, 1\n"
        "  %v1 = phi i32 [%a, entry]\n"
        "  ret i32 %v1\n"
        "}\n",
        4,
        9,
    },
    {
        "operand type mismatch",
        "func @f(i64 %a) -> i32 {\n"
        "entry:\n"
        "  %v0 = add i32 %a, 1\n"
        "  ret i32 0\n"
        "}\n",
        3,
        17,
    },
    {
        "illegal cast",
        "func @f(i32 %a) -> i32 {\n"
        "entry:\n"
        "  %v0 = trunc i32 %a to i64\n"
        "  ret i32 0\n"
        "}\n",
        3,
        9,
    },
    {
        "undefined call target",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  %v0 = call i32 @missing()\n"
        "  ret i32 %v0\n"
        "}\n",
        3,
        18,
    },
    {
        "duplicate block",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  ret i32 0\n"
        "entry:\n"
        "  ret i32 1\n"
        "}\n",
        4,
        1,
    },
    {
        "missing return type",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  ret\n"
        "}\n",
        4,
        1,
    },
    {
        "unexpected character",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  ret i32 0 #\n"
        "}\n",
        3,
        13,
    },
    {
        "switch on float",
        "func @f(f32 %x) -> i32 {\n"
        "entry:\n"
        "  switch f32 %x, [], other\n"
        "other:\n"
        "  ret i32 0\n"
        "}\n",
        3,
        3,
    },
    {
        "wrong argument count",
        "declare i32 @putchar(i32 %c)\n"
        "\n"
        "func @f() -> i32 {\n"
        "entry:\n"
        "  %v0 = call i32 @putchar()\n"
        "  ret i32 %v0\n"
        "}\n",
        5,
        9,
    },
    {
        "void call with result",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  %v0 = call void @f()\n"
        "  ret i32 0\n"
        "}\n",
        3,
        3,
    },
    {
        "duplicate parameter",
        "func @f(i32 %a, i32 %a) -> i32 {\n"
        "entry:\n"
        "  ret i32 %a\n"
        "}\n",
        1,
        21,
    },
    {
        "void operand type",
        "func @f() -> i32 {\n"
        "entry:\n"
        "  %v0 = add void 1, 2\n"
        "  ret i32 0\n"
        "}\n",
        3,
        13,
    },
};

int main(void) {
    int failures = 0;
    size_t count = sizeof(kCases) / sizeof(kCases[0]);
    for (size_t i = 0; i < count; ++i) {
        const ErrorCase *test = &kCases[i];
        WhippetArena *arena = whippet_arena_create();
        WhippetModule *module = NULL;
        WhippetDiagnostic diagnostic;
        bool parsed = whippet_parse_buffer(arena, test->source,
                                           strlen(test->source), &module,
                                           &diagnostic);
        if (parsed) {
            fprintf(stderr, "FAIL %s: input was accepted\n", test->name);
            failures += 1;
        } else if (test->line != 0 &&
                   (diagnostic.line != test->line ||
                    (test->column != 0 && diagnostic.column != test->column))) {
            fprintf(stderr,
                    "FAIL %s: expected %u:%u, got %u:%u (%s)\n", test->name,
                    test->line, test->column, diagnostic.line,
                    diagnostic.column, diagnostic.message);
            failures += 1;
        } else {
            printf("ok %s (%u:%u: %s)\n", test->name, diagnostic.line,
                   diagnostic.column, diagnostic.message);
        }
        whippet_arena_destroy(arena);
    }
    if (failures != 0) {
        fprintf(stderr, "%d diagnostic case(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
