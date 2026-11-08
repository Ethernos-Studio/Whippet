#include "whippet/emit.h"
#include "whippet/ir.h"

#include <stdio.h>
#include <string.h>

static void print_usage(FILE *out) {
    fputs("usage: whippet [options] <file.wir>\n"
          "\n"
          "M0: parse and validate Whippet IR text, then print its canonical form.\n"
          "\n"
          "options:\n"
          "  --check      parse and validate only, produce no output\n"
          "  --version    print the version and exit\n"
          "  -h, --help   print this help and exit\n",
          out);
}

static void print_source_line(FILE *out, const char *source, size_t length,
                              uint32_t line, uint32_t column) {
    if (line == 0) {
        return;
    }
    size_t start = 0;
    uint32_t current = 1;
    while (current < line && start < length) {
        if (source[start] == '\n') {
            current += 1u;
        }
        start += 1u;
    }
    if (current != line) {
        return;
    }
    size_t end = start;
    while (end < length && source[end] != '\n' && source[end] != '\r') {
        end += 1u;
    }
    fprintf(out, "%.*s\n", (int)(end - start), source + start);
    for (uint32_t i = 1; i < column; ++i) {
        fputc(' ', out);
    }
    fputs("^\n", out);
}

static void print_diagnostic(const char *path, const char *source,
                             size_t length,
                             const WhippetDiagnostic *diagnostic) {
    if (diagnostic->line == 0) {
        fprintf(stderr, "whippet: %s\n", diagnostic->message);
        return;
    }
    fprintf(stderr, "%s:%u:%u: error: %s\n", path, diagnostic->line,
            diagnostic->column, diagnostic->message);
    print_source_line(stderr, source, length, diagnostic->line,
                      diagnostic->column);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    bool check_only = false;

    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strcmp(argument, "--version") == 0) {
            puts("whippet 0.1.0 (M0)");
            return 0;
        }
        if (strcmp(argument, "--check") == 0) {
            check_only = true;
            continue;
        }
        if (argument[0] == '-' && argument[1] != '\0') {
            fprintf(stderr, "whippet: unknown option '%s'\n", argument);
            print_usage(stderr);
            return 2;
        }
        if (path != NULL) {
            fputs("whippet: only one input file is supported\n", stderr);
            return 2;
        }
        path = argument;
    }

    if (path == NULL) {
        print_usage(stderr);
        return 2;
    }

    WhippetArena *arena = whippet_arena_create();
    char *source = NULL;
    size_t length = 0;
    if (!whippet_read_file(arena, path, &source, &length)) {
        fprintf(stderr, "whippet: cannot read '%s'\n", path);
        whippet_arena_destroy(arena);
        return 1;
    }

    WhippetModule *module = NULL;
    WhippetDiagnostic diagnostic;
    if (!whippet_parse_buffer(arena, source, length, &module, &diagnostic)) {
        print_diagnostic(path, source, length, &diagnostic);
        whippet_arena_destroy(arena);
        return 1;
    }

    bool ok = true;
    if (!check_only) {
        ok = whippet_emit_text(module, stdout);
        if (ok) {
            ok = fflush(stdout) == 0;
        }
        if (!ok) {
            fputs("whippet: failed to write output\n", stderr);
        }
    }

    whippet_arena_destroy(arena);
    return ok ? 0 : 1;
}
