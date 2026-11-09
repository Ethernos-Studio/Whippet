/*
 * Whippet -- independent compiler backend.
 * Author: dhjs0000
 */

#include "whippet/codegen.h"
#include "whippet/emit.h"
#include "whippet/ir.h"
#include "whippet/passes.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

static void print_usage(FILE *out) {
    fputs("usage: whippet [options] <file.wir>\n"
          "\n"
          "M3: parse Whippet IR and emit Windows x64 output.\n"
          "\n"
          "options:\n"
          "  -S, --emit-asm   emit GNU as text (default)\n"
          "  -c, --emit-obj   emit a COFF object directly (internal assembler)\n"
          "      --emit-ir    print canonical Whippet IR instead\n"
          "  -o <file>        write output to <file>\n"
          "  -O0              disable optimization\n"
          "  -O1              enable O1 passes (default)\n"
          "  --check          parse and validate only, produce no output\n"
          "  --version        print the version and exit\n"
          "  -h, --help       print this help and exit\n",
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

typedef enum OutputMode {
    OUTPUT_ASM = 0,
    OUTPUT_IR,
    OUTPUT_OBJECT
} OutputMode;

static bool emit_output(const WhippetModule *module, FILE *out,
                        OutputMode mode, WhippetDiagnostic *diagnostic) {
    if (mode == OUTPUT_IR) {
        return whippet_emit_text(module, out);
    }
    if (mode == OUTPUT_OBJECT) {
        return whippet_emit_object(module, out, diagnostic);
    }
    return whippet_emit_asm(module, out, diagnostic);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    const char *output_path = NULL;
    bool check_only = false;
    OutputMode output_mode = OUTPUT_ASM;
    int optimization_level = 1;

    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strcmp(argument, "--version") == 0) {
            puts("whippet 0.4.0 (M3)");
            return 0;
        }
        if (strcmp(argument, "--check") == 0) {
            check_only = true;
            continue;
        }
        if (strcmp(argument, "-S") == 0 || strcmp(argument, "--emit-asm") == 0) {
            output_mode = OUTPUT_ASM;
            continue;
        }
        if (strcmp(argument, "-c") == 0 || strcmp(argument, "--emit-obj") == 0) {
            output_mode = OUTPUT_OBJECT;
            continue;
        }
        if (strcmp(argument, "--emit-ir") == 0) {
            output_mode = OUTPUT_IR;
            continue;
        }
        if (strcmp(argument, "-O0") == 0) {
            optimization_level = 0;
            continue;
        }
        if (strcmp(argument, "-O1") == 0) {
            optimization_level = 1;
            continue;
        }
        if (strcmp(argument, "-o") == 0 || strcmp(argument, "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "whippet: '%s' requires a file name\n",
                        argument);
                return 2;
            }
            output_path = argv[++i];
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
    if (!check_only &&
        !whippet_optimize_module(module, optimization_level, &diagnostic)) {
        print_diagnostic(path, source, length, &diagnostic);
        ok = false;
    }
    if (ok && !check_only) {
        if (output_path != NULL) {
            FILE *out = fopen(output_path, "wb");
            if (out == NULL) {
                fprintf(stderr, "whippet: cannot write '%s'\n", output_path);
                ok = false;
            } else {
                ok = emit_output(module, out, output_mode, &diagnostic);
                if (fclose(out) != 0) {
                    ok = false;
                }
                if (!ok) {
                    if (output_mode != OUTPUT_IR) {
                        print_diagnostic(path, source, length, &diagnostic);
                    } else {
                        fprintf(stderr, "whippet: failed to write '%s'\n",
                                output_path);
                    }
                }
            }
        } else {
#if defined(_WIN32)
            /* Keep text and object output byte-exact; no CRT newline rewrite. */
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            ok = emit_output(module, stdout, output_mode, &diagnostic);
            if (ok) {
                ok = fflush(stdout) == 0;
            }
            if (!ok) {
                if (output_mode != OUTPUT_IR) {
                    print_diagnostic(path, source, length, &diagnostic);
                } else {
                    fputs("whippet: failed to write output\n", stderr);
                }
            }
        }
    }

    whippet_arena_destroy(arena);
    return ok ? 0 : 1;
}
