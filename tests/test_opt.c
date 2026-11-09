/*
 * Author: dhjs0000
 *
 * M3 O1 acceptance: constant folding/propagation, copy propagation, DCE and
 * power-of-two strength reduction must be visible in the printed IR.
 */
#include "whippet/ir.h"
#include "whippet/passes.h"

#include <stdio.h>
#include <string.h>

static const char kSource[] =
    "func @opt(i32 %x, i32 %y) -> i32 {\n"
    "entry:\n"
    "  %c1 = add i32 2, 3\n"
    "  %c2 = mul i32 %c1, 4\n"
    "  %c3 = copy i32 %c2\n"
    "  %dead = add i32 %x, %y\n"
    "  %r = add i32 %x, %c3\n"
    "  ret i32 %r\n"
    "}\n"
    "\n"
    "func @strength(i32 %x) -> i32 {\n"
    "entry:\n"
    "  %m = mul i32 %x, 4\n"
    "  ret i32 %m\n"
    "}\n";

static int check_string(const char *label, const char *text,
                        const char *needle, int present) {
    int found = strstr(text, needle) != NULL;
    if (found != present) {
        fprintf(stderr, "FAIL %s: expected '%s' to be %s\n", label, needle,
                present ? "present" : "absent");
        return 1;
    }
    return 0;
}

int main(void) {
    WhippetArena *arena = whippet_arena_create();
    WhippetModule *module = NULL;
    WhippetDiagnostic diagnostic;
    if (!whippet_parse_buffer(arena, kSource, strlen(kSource), &module,
                              &diagnostic)) {
        fprintf(stderr, "FAIL parse: %u:%u: %s\n", diagnostic.line,
                diagnostic.column, diagnostic.message);
        whippet_arena_destroy(arena);
        return 1;
    }
    if (!whippet_optimize_module(module, 1, &diagnostic)) {
        fprintf(stderr, "FAIL optimize: %s\n", diagnostic.message);
        whippet_arena_destroy(arena);
        return 1;
    }
    WhippetText text;
    if (!whippet_print_module(module, arena, &text)) {
        fputs("FAIL print\n", stderr);
        whippet_arena_destroy(arena);
        return 1;
    }
    char *printed = whippet_arena_strndup(arena, text.data, text.length);
    int failures = 0;
    failures += check_string("O1", printed, "add i32 %x, 20", 1);
    failures += check_string("O1", printed, "shl i32 %x, 2", 1);
    failures += check_string("O1", printed, "mul i32", 0);
    failures += check_string("O1", printed, "copy i32", 0);
    failures += check_string("O1", printed, "dead", 0);
    failures += check_string("O1", printed, "5", 0);

    WhippetArena *check = whippet_arena_create();
    WhippetModule *reparsed = NULL;
    if (!whippet_parse_buffer(check, text.data, text.length, &reparsed,
                              &diagnostic)) {
        fprintf(stderr, "FAIL optimized IR reparse: %u:%u: %s\n",
                diagnostic.line, diagnostic.column, diagnostic.message);
        failures += 1;
    } else {
        WhippetText again;
        if (!whippet_print_module(reparsed, check, &again) ||
            again.length != text.length ||
            memcmp(again.data, text.data, text.length) != 0) {
            fputs("FAIL optimized IR is not canonical/round-trippable\n",
                  stderr);
            failures += 1;
        }
    }
    whippet_arena_destroy(check);

    if (failures == 0) {
        printf("ok O1: constants, copies, DCE, strength reduction\n");
    }
    whippet_arena_destroy(arena);
    return failures == 0 ? 0 : 1;
}
