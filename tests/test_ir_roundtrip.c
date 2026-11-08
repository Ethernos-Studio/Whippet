/*
 * Author: dhjs0000
 *
 * M0 acceptance: read .wir -> parse -> canonical print -> byte compare.
 * Every case file in tests/cases/ is stored in canonical form, so a
 * successful round-trip must reproduce the file exactly.
 */
#include "whippet/ir.h"

#include <stdio.h>
#include <string.h>

static int check_file(const char *path) {
    WhippetArena *arena = whippet_arena_create();
    char *source = NULL;
    size_t source_length = 0;
    if (!whippet_read_file(arena, path, &source, &source_length)) {
        fprintf(stderr, "FAIL %s: cannot read file\n", path);
        whippet_arena_destroy(arena);
        return 1;
    }

    WhippetModule *module = NULL;
    WhippetDiagnostic diagnostic;
    if (!whippet_parse_buffer(arena, source, source_length, &module,
                              &diagnostic)) {
        fprintf(stderr, "FAIL %s:%u:%u: %s\n", path, diagnostic.line,
                diagnostic.column, diagnostic.message);
        whippet_arena_destroy(arena);
        return 1;
    }

    WhippetText text;
    if (!whippet_print_module(module, arena, &text)) {
        fprintf(stderr, "FAIL %s: printing failed\n", path);
        whippet_arena_destroy(arena);
        return 1;
    }

    if (text.length != source_length ||
        memcmp(text.data, source, source_length) != 0) {
        size_t mismatch = 0;
        size_t limit = text.length < source_length ? text.length : source_length;
        while (mismatch < limit && text.data[mismatch] == source[mismatch]) {
            mismatch += 1u;
        }
        size_t line = 1;
        for (size_t i = 0; i < mismatch; ++i) {
            if (source[i] == '\n') {
                line += 1u;
            }
        }
        fprintf(stderr,
                "FAIL %s: round-trip mismatch at byte %zu (line %zu)\n"
                "  expected %zu bytes, printed %zu bytes\n",
                path, mismatch, line, source_length, text.length);
        size_t start = mismatch > 40u ? mismatch - 40u : 0u;
        size_t end = mismatch + 40u < source_length ? mismatch + 40u
                                                    : source_length;
        fprintf(stderr, "  input:   %.*s\n", (int)(end - start),
                source + start);
        end = mismatch + 40u < text.length ? mismatch + 40u : text.length;
        fprintf(stderr, "  printed: %.*s\n", (int)(end - start),
                text.data + start);
        whippet_arena_destroy(arena);
        return 1;
    }

    whippet_arena_destroy(arena);
    printf("ok %s\n", path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_ir_roundtrip <case.wir>...\n");
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        failures += check_file(argv[i]);
    }
    if (failures != 0) {
        fprintf(stderr, "%d round-trip case(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
