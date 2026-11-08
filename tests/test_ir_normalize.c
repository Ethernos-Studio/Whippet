/*
 * Author: dhjs0000
 *
 * Lexical conveniences are accepted but normalised: UTF-8 BOM, comments,
 * CRLF, hexadecimal integers and exponent-form floats. The printer must
 * produce the canonical form, which is then stable under a second round-trip.
 */
#include "whippet/ir.h"

#include <stdio.h>
#include <string.h>

static const char kSource[] =
    "\xEF\xBB\xBF; leading comment\r\n"
    "func @f(i32 %a) -> i32 { // function\r\n"
    "entry: ; block\r\n"
    "  %v0 = add i32 0x10, %a\r\n"
    "  %v1 = fadd f32 1e-3, 2.0\r\n"
    "  ret i32 %v0\r\n"
    "}\r\n";

static const char kExpected[] =
    "func @f(i32 %a) -> i32 {\n"
    "entry:\n"
    "  %v0 = add i32 16, %a\n"
    "  %v1 = fadd f32 0.001, 2.0\n"
    "  ret i32 %v0\n"
    "}\n";

int main(void) {
    WhippetArena *arena = whippet_arena_create();
    WhippetModule *module = NULL;
    WhippetDiagnostic diagnostic;
    if (!whippet_parse_buffer(arena, kSource, sizeof(kSource) - 1u, &module,
                              &diagnostic)) {
        fprintf(stderr, "FAIL normalisation: %u:%u: %s\n", diagnostic.line,
                diagnostic.column, diagnostic.message);
        whippet_arena_destroy(arena);
        return 1;
    }

    WhippetText text;
    if (!whippet_print_module(module, arena, &text)) {
        fprintf(stderr, "FAIL normalisation: print failed\n");
        whippet_arena_destroy(arena);
        return 1;
    }
    if (text.length != sizeof(kExpected) - 1u ||
        memcmp(text.data, kExpected, sizeof(kExpected) - 1u) != 0) {
        fprintf(stderr, "FAIL normalisation: canonical text differs\n");
        fprintf(stderr, "printed:\n%.*s", (int)text.length, text.data);
        whippet_arena_destroy(arena);
        return 1;
    }

    /* The canonical text must round-trip byte-for-byte a second time. */
    WhippetModule *second = NULL;
    if (!whippet_parse_buffer(arena, text.data, text.length, &second,
                              &diagnostic)) {
        fprintf(stderr, "FAIL normalisation: canonical text rejected\n");
        whippet_arena_destroy(arena);
        return 1;
    }
    WhippetText again;
    if (!whippet_print_module(second, arena, &again) ||
        again.length != text.length ||
        memcmp(again.data, text.data, text.length) != 0) {
        fprintf(stderr, "FAIL normalisation: second round-trip differs\n");
        whippet_arena_destroy(arena);
        return 1;
    }

    whippet_arena_destroy(arena);
    puts("ok ir normalisation");
    return 0;
}
