/*
 * Author: dhjs0000
 *
 * M1 codegen smoke tests: every supported integer case must produce GNU as
 * text, and the M2/M4-only constructs must fail with a located diagnostic.
 */
#include "whippet/codegen.h"
#include "whippet/ir.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void fail(const char *what) {
    fprintf(stderr, "FAIL %s\n", what);
    failures += 1;
}

static int check_file(const char *path, const char *needle,
                      const char *second_needle) {
    WhippetArena *arena = whippet_arena_create();
    char *source = NULL;
    size_t length = 0;
    if (!whippet_read_file(arena, path, &source, &length)) {
        fprintf(stderr, "FAIL %s: cannot read\n", path);
        whippet_arena_destroy(arena);
        return 1;
    }
    WhippetModule *module = NULL;
    WhippetDiagnostic diagnostic;
    if (!whippet_parse_buffer(arena, source, length, &module, &diagnostic)) {
        fprintf(stderr, "FAIL %s:%u:%u: %s\n", path, diagnostic.line,
                diagnostic.column, diagnostic.message);
        whippet_arena_destroy(arena);
        return 1;
    }

    FILE *file = tmpfile();
    if (file == NULL) {
        fail("tmpfile");
        whippet_arena_destroy(arena);
        return 1;
    }
    bool emitted = whippet_emit_asm(module, file, &diagnostic);
    if (!emitted) {
        fprintf(stderr, "FAIL %s: %u:%u: %s\n", path, diagnostic.line,
                diagnostic.column, diagnostic.message);
        fclose(file);
        whippet_arena_destroy(arena);
        return 1;
    }
    fflush(file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fail("empty assembly");
        fclose(file);
        whippet_arena_destroy(arena);
        return 1;
    }
    char *text = (char *)whippet_arena_alloc(arena, (size_t)size + 1u, 1);
    fread(text, 1, (size_t)size, file);
    text[size] = '\0';
    fclose(file);

    if (strstr(text, ".intel_syntax noprefix") == NULL ||
        strstr(text, needle) == NULL ||
        (second_needle != NULL && strstr(text, second_needle) == NULL) ||
        strstr(text, "internal:") != NULL) {
        fprintf(stderr, "FAIL %s: missing expected assembly\n%s\n", path,
                text);
        whippet_arena_destroy(arena);
        return 1;
    }
    whippet_arena_destroy(arena);
    printf("ok asm %s\n", path);
    return 0;
}

static int expect_failure(const char *name, const char *source,
                          const char *needle) {
    WhippetArena *arena = whippet_arena_create();
    WhippetModule *module = NULL;
    WhippetDiagnostic diagnostic;
    if (!whippet_parse_buffer(arena, source, strlen(source), &module,
                              &diagnostic)) {
        fprintf(stderr, "FAIL %s: IR unexpectedly rejected: %s\n", name,
                diagnostic.message);
        whippet_arena_destroy(arena);
        return 1;
    }
    FILE *file = tmpfile();
    if (file == NULL) {
        fail("tmpfile");
        whippet_arena_destroy(arena);
        return 1;
    }
    bool emitted = whippet_emit_asm(module, file, &diagnostic);
    fclose(file);
    if (emitted) {
        fprintf(stderr, "FAIL %s: codegen unexpectedly succeeded\n", name);
        whippet_arena_destroy(arena);
        return 1;
    }
    if (strstr(diagnostic.message, needle) == NULL) {
        fprintf(stderr, "FAIL %s: diagnostic '%s' does not mention '%s'\n",
                name, diagnostic.message, needle);
        whippet_arena_destroy(arena);
        return 1;
    }
    printf("ok asm error %s (%s)\n", name, diagnostic.message);
    whippet_arena_destroy(arena);
    return 0;
}

static const char kCallSource[] =
    "declare void @abort()\n"
    "\n"
    "func @f() -> i32 {\n"
    "entry:\n"
    "  call void @abort()\n"
    "  ret i32 0\n"
    "}\n";

static const char kMemorySource[] =
    "func @f() -> i32 {\n"
    "entry:\n"
    "  %p = alloca i32\n"
    "  ret i32 0\n"
    "}\n";

static const char kFloatSource[] =
    "func @f(f32 %x) -> i32 {\n"
    "entry:\n"
    "  ret i32 0\n"
    "}\n";

static const char kManyParamsSource[] =
    "func @f(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e) -> i32 {\n"
    "entry:\n"
    "  ret i32 %a\n"
    "}\n";

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr,
                "usage: test_asm <basic.wir> <loops.wir> <int_ops.wir> "
                "<control.wir>\n");
        return 2;
    }
    failures += check_file(argv[1], "imul", "ret");
    failures += check_file(argv[2], "jne", ".Lwhippet_");
    failures += check_file(argv[3], "push r", "mov qword ptr [rsp");
    failures += check_file(argv[4], "cmp rax", "je ");

    failures += expect_failure("call", kCallSource, "M2");
    failures += expect_failure("memory", kMemorySource, "M2");
    failures += expect_failure("float", kFloatSource, "f32");
    failures += expect_failure("params", kManyParamsSource, "4");

    if (failures != 0) {
        fprintf(stderr, "%d codegen test(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
