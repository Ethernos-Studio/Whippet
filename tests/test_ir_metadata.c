/*
 * Author: dhjs0000
 *
 * Self-checks for the public IR tables and module API. These catch a missing
 * opcode table entry (which would otherwise be a NULL name) and verify that
 * every printed mnemonic parses back to the same enum value.
 */
#include "whippet/ir.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void fail(const char *what) {
    fprintf(stderr, "FAIL %s\n", what);
    failures += 1;
}

int main(void) {
    for (int type = 0; type < WHIPPET_TYPE_COUNT; ++type) {
        const char *name = whippet_type_name((WhippetType)type);
        if (name == NULL || strcmp(name, "invalid") == 0) {
            fail("type name");
            continue;
        }
        WhippetType parsed = WHIPPET_TYPE_VOID;
        if (!whippet_type_from_string(name, strlen(name), &parsed) ||
            parsed != (WhippetType)type) {
            fail("type round-trip");
        }
    }

    for (int opcode = 1; opcode < WHIPPET_OP_COUNT; ++opcode) {
        const char *name = whippet_opcode_name((WhippetOpcode)opcode);
        if (name == NULL || strcmp(name, "invalid") == 0) {
            fail("opcode name");
            continue;
        }
        WhippetOpcode parsed = WHIPPET_OP_INVALID;
        if (!whippet_opcode_from_string(name, strlen(name), &parsed) ||
            parsed != (WhippetOpcode)opcode) {
            fail("opcode round-trip");
        }
    }

    if (!whippet_opcode_has_result(WHIPPET_OP_ADD) ||
        !whippet_opcode_has_result(WHIPPET_OP_CALL) ||
        whippet_opcode_has_result(WHIPPET_OP_RET) ||
        whippet_opcode_has_result(WHIPPET_OP_BR) ||
        whippet_opcode_has_result(WHIPPET_OP_CONDBR) ||
        whippet_opcode_has_result(WHIPPET_OP_SWITCH) ||
        whippet_opcode_has_result(WHIPPET_OP_STORE)) {
        fail("opcode result flags");
    }

    static const WhippetOpcode comparisons[] = {WHIPPET_OP_ICMP, WHIPPET_OP_FCMP};
    for (size_t c = 0; c < sizeof(comparisons) / sizeof(comparisons[0]); ++c) {
        for (uint8_t predicate = 0; predicate < 32; ++predicate) {
            const char *name =
                whippet_predicate_name(comparisons[c], predicate);
            uint8_t parsed = 0xFF;
            bool ok = whippet_predicate_from_string(
                comparisons[c], name == NULL ? "" : name,
                name == NULL ? 0 : strlen(name), &parsed);
            if (name == NULL) {
                if (ok) {
                    fail("out-of-range predicate accepted");
                }
            } else if (!ok || parsed != predicate) {
                fail("predicate round-trip");
            }
        }
    }

    WhippetArena *arena = whippet_arena_create();
    WhippetModule *module = NULL;
    WhippetDiagnostic diagnostic;
    if (!whippet_parse_buffer(arena, "", 0, &module, &diagnostic)) {
        fail("empty module rejected");
    } else if (whippet_module_function_count(module) != 0) {
        fail("empty module function count");
    }
    whippet_arena_destroy(arena);

    static const char source[] =
        "func @one() -> i32 {\n"
        "entry:\n"
        "  ret i32 0\n"
        "}\n";
    arena = whippet_arena_create();
    if (!whippet_parse_buffer(arena, source, sizeof(source) - 1u, &module,
                              &diagnostic)) {
        fprintf(stderr, "FAIL minimal module: %s\n", diagnostic.message);
        failures += 1;
    } else if (whippet_module_function_count(module) != 1) {
        fail("function count");
    }
    whippet_arena_destroy(arena);

    if (failures == 0) {
        puts("ok ir metadata");
    }
    return failures == 0 ? 0 : 1;
}
