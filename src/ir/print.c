/*
 * Canonical printer for Whippet IR.
 *
 * The output of this file is the round-trip reference: parsing it again must
 * produce the same module, and printing that module must reproduce these
 * bytes exactly. Formatting rules:
 *   - top-level items separated by one blank line;
 *   - block labels at column 0, instructions indented by two spaces;
 *   - no blank lines between blocks;
 *   - one trailing newline after the last top-level item;
 *   - comments and redundant blank lines are not preserved;
 *   - numbers are printed in decimal / shortest round-trip form.
 */
#include "whippet/ir.h"

#include "ir_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct Printer {
    const WhippetModule *module;
    WhippetArena *arena;
    WhippetText *text;
} Printer;

static void text_grow(Printer *printer, size_t needed) {
    WhippetText *text = printer->text;
    if (text->length + needed + 1u <= text->capacity) {
        return;
    }
    size_t capacity = text->capacity == 0 ? 512u : text->capacity;
    while (capacity < text->length + needed + 1u) {
        capacity *= 2u;
    }
    char *fresh = (char *)whippet_arena_alloc(printer->arena, capacity, 8);
    if (text->length != 0) {
        memcpy(fresh, text->data, text->length);
    }
    text->data = fresh;
    text->capacity = capacity;
}

static void text_append_n(Printer *printer, const char *data, size_t length) {
    if (length == 0) {
        return;
    }
    text_grow(printer, length);
    memcpy(printer->text->data + printer->text->length, data, length);
    printer->text->length += length;
    printer->text->data[printer->text->length] = '\0';
}

static void text_append(Printer *printer, const char *text) {
    text_append_n(printer, text, strlen(text));
}

static void text_printf(Printer *printer, const char *format, ...) {
    char buffer[256];
    va_list arguments;
    va_start(arguments, format);
    int needed = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (needed < 0) {
        return;
    }
    if ((size_t)needed < sizeof(buffer)) {
        text_append_n(printer, buffer, (size_t)needed);
        return;
    }
    char *large =
        (char *)whippet_arena_alloc(printer->arena, (size_t)needed + 1u, 1);
    va_start(arguments, format);
    vsnprintf(large, (size_t)needed + 1u, format, arguments);
    va_end(arguments);
    text_append_n(printer, large, (size_t)needed);
}

/* Shortest decimal form that parses back to the same value. */
static void format_float(char *buffer, size_t size, double value,
                         WhippetType type) {
    if (type == WHIPPET_TYPE_F32) {
        float single = (float)value;
        for (int precision = 1; precision <= 9; ++precision) {
            snprintf(buffer, size, "%.*g", precision, (double)single);
            if (strtof(buffer, NULL) == single) {
                break;
            }
        }
    } else {
        for (int precision = 1; precision <= 17; ++precision) {
            snprintf(buffer, size, "%.*g", precision, value);
            if (strtod(buffer, NULL) == value) {
                break;
            }
        }
    }
    /* Keep the literal recognisable as floating point: 1 -> 1.0 */
    if (strpbrk(buffer, ".eE") == NULL && strcmp(buffer, "inf") != 0 &&
        strcmp(buffer, "-inf") != 0 && strcmp(buffer, "nan") != 0) {
        size_t length = strlen(buffer);
        if (length + 2u < size) {
            buffer[length] = '.';
            buffer[length + 1u] = '0';
            buffer[length + 2u] = '\0';
        }
    }
}

static void print_operand(Printer *printer, const WhippetOperand *operand) {
    switch (operand->kind) {
    case WHIPPET_OPERAND_VALUE:
        text_append(printer, "%");
        text_append(printer,
                    whippet_arena_string(printer->module->arena, operand->name));
        break;
    case WHIPPET_OPERAND_SYMBOL:
        text_append(printer, "@");
        text_append(printer,
                    whippet_arena_string(printer->module->arena, operand->name));
        break;
    case WHIPPET_OPERAND_BLOCK:
        text_append(printer,
                    whippet_arena_string(printer->module->arena, operand->name));
        break;
    case WHIPPET_OPERAND_INT:
        text_printf(printer, "%lld", (long long)(int64_t)operand->imm);
        break;
    case WHIPPET_OPERAND_FLOAT: {
        double value;
        memcpy(&value, &operand->imm, sizeof(value));
        char buffer[64];
        format_float(buffer, sizeof(buffer), value,
                     (WhippetType)operand->type);
        text_append(printer, buffer);
        break;
    }
    default:
        text_append(printer, "<invalid>");
        break;
    }
}

static void print_typed_operand(Printer *printer, const WhippetOperand *operand) {
    text_append(printer, whippet_type_name((WhippetType)operand->type));
    text_append(printer, " ");
    print_operand(printer, operand);
}

static void print_value(Printer *printer, const WhippetFunction *function,
                        uint32_t value_id) {
    text_append(printer, "%");
    text_append(printer,
                whippet_arena_string(printer->module->arena,
                                     function->values[value_id - 1u].name));
}

static void print_instruction(Printer *printer, const WhippetFunction *function,
                              const WhippetInstr *instruction) {
    const WhippetOperand *operands =
        whippet_instr_operands(printer->module, instruction);
    WhippetOpcode opcode = (WhippetOpcode)instruction->opcode;
    WhippetOpcodeKind kind = whippet_opcode_kind(opcode);
    uint16_t count = instruction->operand_count;

    text_append(printer, "  ");
    if (instruction->result != 0) {
        print_value(printer, function, instruction->result);
        text_append(printer, " = ");
    }
    text_append(printer, whippet_opcode_name(opcode));

    switch (kind) {
    case WHIPPET_KIND_TERMINATOR:
        switch (opcode) {
        case WHIPPET_OP_RET:
            if (count == 1) {
                text_append(printer, " ");
                print_typed_operand(printer, &operands[0]);
            }
            break;
        case WHIPPET_OP_BR:
            text_append(printer, " ");
            print_operand(printer, &operands[0]);
            break;
        case WHIPPET_OP_CONDBR:
            text_append(printer, " ");
            print_typed_operand(printer, &operands[0]);
            text_append(printer, ", ");
            print_operand(printer, &operands[1]);
            text_append(printer, ", ");
            print_operand(printer, &operands[2]);
            break;
        case WHIPPET_OP_SWITCH:
            text_append(printer, " ");
            print_typed_operand(printer, &operands[0]);
            text_append(printer, ", [");
            for (uint16_t i = 1; i + 1u < count; ++i) {
                if (i > 1u) {
                    text_append(printer, ", ");
                }
                print_operand(printer, &operands[i]);
            }
            text_append(printer, "], ");
            print_operand(printer, &operands[count - 1u]);
            break;
        default:
            break;
        }
        break;

    case WHIPPET_KIND_BINARY_INT:
    case WHIPPET_KIND_BINARY_FP:
        text_append(printer, " ");
        print_typed_operand(printer, &operands[0]);
        text_append(printer, ", ");
        print_operand(printer, &operands[1]);
        break;

    case WHIPPET_KIND_UNARY_INT:
    case WHIPPET_KIND_UNARY_FP:
        text_append(printer, " ");
        print_typed_operand(printer, &operands[0]);
        break;

    case WHIPPET_KIND_ICMP:
    case WHIPPET_KIND_FCMP:
        text_printf(printer, " %s ",
                    whippet_predicate_name(opcode, instruction->aux));
        print_typed_operand(printer, &operands[0]);
        text_append(printer, ", ");
        print_operand(printer, &operands[1]);
        break;

    case WHIPPET_KIND_ALLOCA:
        text_append(printer, " ");
        text_append(printer,
                    whippet_type_name((WhippetType)instruction->aux));
        break;

    case WHIPPET_KIND_LOAD:
        text_append(printer, " ");
        text_append(printer,
                    whippet_type_name((WhippetType)instruction->result_type));
        text_append(printer, ", ptr ");
        print_operand(printer, &operands[0]);
        break;

    case WHIPPET_KIND_STORE:
        text_append(printer, " ");
        print_typed_operand(printer, &operands[0]);
        text_append(printer, ", ptr ");
        print_operand(printer, &operands[1]);
        break;

    case WHIPPET_KIND_PTRADD:
        text_append(printer, " ptr ");
        print_operand(printer, &operands[0]);
        text_append(printer, ", ");
        print_typed_operand(printer, &operands[1]);
        break;

    case WHIPPET_KIND_PHI:
        text_append(printer, " ");
        text_append(printer,
                    whippet_type_name((WhippetType)instruction->result_type));
        for (uint16_t i = 0; i + 1u < count; i += 2u) {
            text_append(printer, i == 0 ? " [" : ", [");
            print_operand(printer, &operands[i]);
            text_append(printer, ", ");
            print_operand(printer, &operands[i + 1u]);
            text_append(printer, "]");
        }
        break;

    case WHIPPET_KIND_COPY:
        text_append(printer, " ");
        print_typed_operand(printer, &operands[0]);
        break;

    case WHIPPET_KIND_SELECT:
        text_append(printer, " ");
        print_typed_operand(printer, &operands[0]);
        text_append(printer, ", ");
        print_typed_operand(printer, &operands[1]);
        text_append(printer, ", ");
        print_typed_operand(printer, &operands[2]);
        break;

    case WHIPPET_KIND_CALL:
        text_append(printer, " ");
        text_append(printer,
                    whippet_type_name((WhippetType)instruction->result_type));
        text_append(printer, " ");
        print_operand(printer, &operands[0]);
        text_append(printer, "(");
        for (uint16_t i = 1; i < count; ++i) {
            if (i > 1u) {
                text_append(printer, ", ");
            }
            print_typed_operand(printer, &operands[i]);
        }
        text_append(printer, ")");
        break;

    case WHIPPET_KIND_CAST:
        text_append(printer, " ");
        text_append(printer,
                    whippet_type_name((WhippetType)instruction->aux));
        text_append(printer, " ");
        print_operand(printer, &operands[0]);
        text_append(printer, " to ");
        text_append(printer,
                    whippet_type_name((WhippetType)instruction->result_type));
        break;

    default:
        text_append(printer, " <invalid>");
        break;
    }
    text_append(printer, "\n");
}

static void print_parameters(Printer *printer,
                             const WhippetFunction *function) {
    for (uint32_t i = 0; i < function->param_count; ++i) {
        if (i != 0) {
            text_append(printer, ", ");
        }
        text_append(printer,
                    whippet_type_name((WhippetType)function->params[i].type));
        text_append(printer, " %");
        text_append(printer,
                    whippet_arena_string(printer->module->arena,
                                         function->params[i].name));
    }
}

static void print_function(Printer *printer, const WhippetFunction *function) {
    if (function->is_declaration) {
        text_append(printer, "declare ");
        text_append(printer,
                    whippet_type_name((WhippetType)function->return_type));
        text_append(printer, " @");
        text_append(printer,
                    whippet_arena_string(printer->module->arena, function->name));
        text_append(printer, "(");
        print_parameters(printer, function);
        text_append(printer, ")\n");
        return;
    }

    text_append(printer, "func @");
    text_append(printer,
                whippet_arena_string(printer->module->arena, function->name));
    text_append(printer, "(");
    print_parameters(printer, function);
    text_append(printer, ")");
    if (function->return_type != WHIPPET_TYPE_VOID) {
        text_append(printer, " -> ");
        text_append(printer,
                    whippet_type_name((WhippetType)function->return_type));
    }
    text_append(printer, " {\n");
    for (const WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        text_append(printer,
                    whippet_arena_string(printer->module->arena, block->name));
        text_append(printer, ":\n");
        const WhippetInstr *instruction = NULL;
        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(printer->module, handle);
            print_instruction(printer, function, instruction);
        }
    }
    text_append(printer, "}\n");
}

bool whippet_print_module(const WhippetModule *module, WhippetArena *arena,
                          WhippetText *out) {
    Printer printer;
    printer.module = module;
    printer.arena = arena;
    printer.text = out;
    out->data = NULL;
    out->length = 0;
    out->capacity = 0;
    text_grow(&printer, 0);

    bool first = true;
    for (const WhippetFunction *function = module->first_function;
         function != NULL; function = function->next) {
        if (!first) {
            text_append(&printer, "\n");
        }
        first = false;
        print_function(&printer, function);
    }
    out->data[out->length] = '\0';
    return true;
}

bool whippet_print_module_to_file(const WhippetModule *module, FILE *file) {
    WhippetArena *arena = whippet_arena_create();
    WhippetText text;
    bool ok = whippet_print_module(module, arena, &text);
    if (ok) {
        ok = fwrite(text.data, 1, text.length, file) == text.length;
    }
    whippet_arena_destroy(arena);
    return ok;
}
