/*
 * Author: dhjs0000
 *
 * M3 internal assembler + COFF writer for the instruction subset produced by
 * the M1/M2 backend. The backend still emits canonical GNU as text; this file
 * consumes that text, encodes the small instruction set it actually uses, and
 * writes a Windows x64 COFF object file without spawning clang/as.
 */
#include "whippet/codegen.h"

#include "backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small byte buffer                                                   */
/* ------------------------------------------------------------------ */

typedef struct CodeBuffer {
    unsigned char *data;
    size_t length;
    size_t capacity;
    WhippetArena *arena;
} CodeBuffer;

static void code_reserve(CodeBuffer *buffer, size_t extra) {
    if (buffer->length + extra <= buffer->capacity) {
        return;
    }
    size_t capacity = buffer->capacity == 0 ? 1024u : buffer->capacity;
    while (capacity < buffer->length + extra) {
        capacity *= 2u;
    }
    unsigned char *fresh =
        (unsigned char *)whippet_arena_alloc(buffer->arena, capacity, 8);
    if (buffer->length != 0) {
        memcpy(fresh, buffer->data, buffer->length);
    }
    buffer->data = fresh;
    buffer->capacity = capacity;
}

static void code_u8(CodeBuffer *buffer, uint8_t value) {
    code_reserve(buffer, 1);
    buffer->data[buffer->length++] = value;
}

static void code_u16(CodeBuffer *buffer, uint16_t value) {
    code_u8(buffer, (uint8_t)(value & 0xFF));
    code_u8(buffer, (uint8_t)(value >> 8));
}

static void code_u32(CodeBuffer *buffer, uint32_t value) {
    code_u16(buffer, (uint16_t)(value & 0xFFFF));
    code_u16(buffer, (uint16_t)(value >> 16));
}

static void code_u64(CodeBuffer *buffer, uint64_t value) {
    code_u32(buffer, (uint32_t)(value & 0xFFFFFFFFu));
    code_u32(buffer, (uint32_t)(value >> 32));
}

/* ------------------------------------------------------------------ */
/* Assembler state                                                     */
/* ------------------------------------------------------------------ */

typedef struct CoffLabel {
    char *name;
    uint32_t offset;
    bool global;
} CoffLabel;

typedef struct CoffFixup {
    uint32_t offset; /* start of the rel32 field */
    char *name;
    bool is_call;
} CoffFixup;

typedef struct CoffReloc {
    uint32_t offset;
    uint32_t symbol_index;
} CoffReloc;

typedef struct CoffSymbol {
    char *name;
    uint32_t value;
    int16_t section;
} CoffSymbol;

typedef struct CoffAssembler {
    WhippetArena *arena;
    CodeBuffer code;
    WhippetDiagnostic *diagnostic;

    CoffLabel *labels;
    uint32_t label_count;
    uint32_t label_capacity;

    CoffFixup *fixups;
    uint32_t fixup_count;
    uint32_t fixup_capacity;

    CoffReloc *relocations;
    uint32_t relocation_count;
    uint32_t relocation_capacity;

    CoffSymbol *symbols;
    uint32_t symbol_count;
    uint32_t symbol_capacity;

    char **globals;
    uint32_t global_count;
    uint32_t global_capacity;
} CoffAssembler;

static bool assembler_error(CoffAssembler *assembler, const char *format, ...) {
    if (assembler->diagnostic != NULL) {
        assembler->diagnostic->line = 0;
        assembler->diagnostic->column = 0;
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(assembler->diagnostic->message,
                  sizeof(assembler->diagnostic->message), format, arguments);
        va_end(arguments);
    }
    return false;
}

static char *trim_text(char *text) {
    while (*text != '\0' && (*text == ' ' || *text == '\t' || *text == '\r')) {
        text += 1;
    }
    size_t length = strlen(text);
    while (length != 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t' ||
            text[length - 1] == '\r')) {
        text[--length] = '\0';
    }
    return text;
}

static char *arena_dup(CoffAssembler *assembler, const char *text) {
    return whippet_arena_strndup(assembler->arena, text, strlen(text));
}

/* ------------------------------------------------------------------ */
/* Registers and operands                                              */
/* ------------------------------------------------------------------ */

typedef struct RegisterInfo {
    const char *name;
    uint8_t id;
    uint8_t width;
} RegisterInfo;

static const RegisterInfo kRegisters[] = {
    {"rax", 0, 8},  {"rcx", 1, 8},  {"rdx", 2, 8},  {"rbx", 3, 8},
    {"rsp", 4, 8},  {"rbp", 5, 8},  {"rsi", 6, 8},  {"rdi", 7, 8},
    {"r8", 8, 8},   {"r9", 9, 8},   {"r10", 10, 8}, {"r11", 11, 8},
    {"r12", 12, 8}, {"r13", 13, 8}, {"r14", 14, 8}, {"r15", 15, 8},

    {"eax", 0, 4},  {"ecx", 1, 4},  {"edx", 2, 4},  {"ebx", 3, 4},
    {"esp", 4, 4},  {"ebp", 5, 4},  {"esi", 6, 4},  {"edi", 7, 4},
    {"r8d", 8, 4},  {"r9d", 9, 4},  {"r10d", 10, 4},{"r11d", 11, 4},
    {"r12d", 12, 4},{"r13d", 13, 4},{"r14d", 14, 4},{"r15d", 15, 4},

    {"ax", 0, 2},   {"cx", 1, 2},   {"dx", 2, 2},   {"bx", 3, 2},
    {"bp", 5, 2},   {"si", 6, 2},   {"di", 7, 2},   {"r8w", 8, 2},
    {"r9w", 9, 2},  {"r10w", 10, 2},{"r11w", 11, 2},{"r12w", 12, 2},
    {"r13w", 13, 2},{"r14w", 14, 2},{"r15w", 15, 2},

    {"al", 0, 1},   {"cl", 1, 1},   {"dl", 2, 1},   {"bl", 3, 1},
    {"bpl", 5, 1},  {"sil", 6, 1},  {"dil", 7, 1},  {"r8b", 8, 1},
    {"r9b", 9, 1},  {"r10b", 10, 1},{"r11b", 11, 1},{"r12b", 12, 1},
    {"r13b", 13, 1},{"r14b", 14, 1},{"r15b", 15, 1},
};

typedef enum OperandKind {
    OPERAND_INVALID = 0,
    OPERAND_REG,
    OPERAND_IMM,
    OPERAND_MEMORY,
    OPERAND_NAME
} OperandKind;

typedef struct AsmOperand {
    OperandKind kind;
    int reg;
    int width;
    uint64_t immediate;
    int base;
    int32_t displacement;
    int memory_size;
    const char *name;
} AsmOperand;

static const RegisterInfo *find_register_n(const char *text, size_t length) {
    for (size_t i = 0; i < sizeof(kRegisters) / sizeof(kRegisters[0]); ++i) {
        if (strlen(kRegisters[i].name) == length &&
            memcmp(kRegisters[i].name, text, length) == 0) {
            return &kRegisters[i];
        }
    }
    return NULL;
}

static const RegisterInfo *find_register(const char *text) {
    return find_register_n(text, strlen(text));
}

static int memory_size_prefix(const char **text) {
    static const struct {
        const char *prefix;
        int size;
    } kPrefixes[] = {
        {"byte ptr ", 1},
        {"word ptr ", 2},
        {"dword ptr ", 4},
        {"qword ptr ", 8},
    };
    for (size_t i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); ++i) {
        size_t length = strlen(kPrefixes[i].prefix);
        if (strncmp(*text, kPrefixes[i].prefix, length) == 0) {
            *text += length;
            return kPrefixes[i].size;
        }
    }
    return 0;
}

static bool parse_memory_operand(CoffAssembler *assembler, char *text,
                                 int memory_size, AsmOperand *out) {
    if (*text != '[') {
        return assembler_error(assembler, "expected '[' in memory operand");
    }
    char *close = strchr(text, ']');
    if (close == NULL) {
        return assembler_error(assembler, "unterminated memory operand");
    }
    char *extra = trim_text(close + 1);
    if (*extra != '\0') {
        return assembler_error(assembler, "junk after memory operand");
    }
    *close = '\0';
    char *content = trim_text(text + 1);
    char *cursor = content;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
           *cursor != '+' && *cursor != '-') {
        cursor += 1;
    }
    size_t name_length = (size_t)(cursor - content);
    const RegisterInfo *base = find_register_n(content, name_length);
    if (base == NULL || base->width != 8) {
        return assembler_error(assembler, "invalid memory base register");
    }
    int32_t displacement = 0;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }
    if (*cursor == '+' || *cursor == '-') {
        int sign = *cursor == '-' ? -1 : 1;
        cursor += 1;
        char *end = NULL;
        long value = strtol(cursor, &end, 0);
        if (end == cursor || *trim_text(end) != '\0') {
            return assembler_error(assembler, "invalid memory displacement");
        }
        displacement = (int32_t)(value * sign);
    } else if (*cursor != '\0') {
        return assembler_error(assembler, "invalid memory operand");
    }

    memset(out, 0, sizeof(*out));
    out->kind = OPERAND_MEMORY;
    out->base = base->id;
    out->displacement = displacement;
    out->memory_size = memory_size;
    out->width = memory_size;
    return true;
}

static bool parse_operand(CoffAssembler *assembler, char *text, AsmOperand *out) {
    text = trim_text(text);
    if (*text == '\0') {
        return assembler_error(assembler, "empty operand");
    }
    memset(out, 0, sizeof(*out));

    const char *memory_text = text;
    int memory_size = memory_size_prefix(&memory_text);
    if (memory_size != 0) {
        memory_text = trim_text((char *)memory_text);
        return parse_memory_operand(assembler, (char *)memory_text, memory_size,
                                    out);
    }
    if (*text == '[') {
        return parse_memory_operand(assembler, text, 0, out);
    }

    const RegisterInfo *reg = find_register(text);
    if (reg != NULL) {
        out->kind = OPERAND_REG;
        out->reg = reg->id;
        out->width = reg->width;
        return true;
    }

    if ((text[0] >= '0' && text[0] <= '9') || text[0] == '-') {
        char *end = NULL;
        unsigned long long value = strtoull(text, &end, 0);
        if (end == text || *end != '\0') {
            return assembler_error(assembler, "invalid immediate '%s'", text);
        }
        out->kind = OPERAND_IMM;
        out->immediate = (uint64_t)value;
        return true;
    }

    out->kind = OPERAND_NAME;
    out->name = text;
    return true;
}

/* ------------------------------------------------------------------ */
/* Encoding helpers                                                    */
/* ------------------------------------------------------------------ */

static void emit_rex(CoffAssembler *assembler, bool wide, int reg, int base,
                     bool force) {
    uint8_t rex = 0x40;
    if (wide) {
        rex |= 0x08;
    }
    if (reg >= 8) {
        rex |= 0x04;
    }
    if (base >= 8) {
        rex |= 0x01;
    }
    if (rex != 0x40 || force) {
        code_u8(&assembler->code, rex);
    }
}

static void emit_modrm(CoffAssembler *assembler, int mod, int reg, int rm) {
    code_u8(&assembler->code,
            (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

static void emit_memory(CoffAssembler *assembler, int reg, const AsmOperand *mem) {
    int base = mem->base;
    int32_t displacement = mem->displacement;
    int low = base & 7;
    if (base == 4) { /* rsp/r12: SIB with no index */
        int mod = (displacement >= -128 && displacement <= 127) ? 1 : 2;
        emit_modrm(assembler, mod, reg, 4);
        code_u8(&assembler->code, 0x24);
        if (mod == 1) {
            code_u8(&assembler->code, (uint8_t)(int8_t)displacement);
        } else {
            code_u32(&assembler->code, (uint32_t)displacement);
        }
        return;
    }
    int mod;
    if (displacement == 0 && low != 5) {
        mod = 0;
    } else if (displacement >= -128 && displacement <= 127) {
        mod = 1;
    } else {
        mod = 2;
    }
    emit_modrm(assembler, mod, reg, low);
    if (mod == 1) {
        code_u8(&assembler->code, (uint8_t)(int8_t)displacement);
    } else if (mod == 2) {
        code_u32(&assembler->code, (uint32_t)displacement);
    }
}

static bool need_rex_for_byte_register(int reg) {
    return reg >= 4;
}

static bool encode_expected(CoffAssembler *assembler, const char *mnemonic,
                            int count, int expected) {
    if (count != expected) {
        return assembler_error(assembler, "'%s' expects %d operands",
                               mnemonic, expected);
    }
    return true;
}

static bool encode_mov(CoffAssembler *assembler, const AsmOperand *ops,
                       int count) {
    if (!encode_expected(assembler, "mov", count, 2)) {
        return false;
    }
    const AsmOperand *destination = &ops[0];
    const AsmOperand *source = &ops[1];

    if (destination->kind == OPERAND_REG && source->kind == OPERAND_REG) {
        if (destination->width != source->width) {
            return assembler_error(assembler, "mov register width mismatch");
        }
        int width = destination->width;
        if (width == 2) {
            code_u8(&assembler->code, 0x66);
        }
        emit_rex(assembler, width == 8, source->reg, destination->reg,
                 width == 1 && (need_rex_for_byte_register(source->reg) ||
                                need_rex_for_byte_register(
                                    destination->reg)));
        code_u8(&assembler->code, width == 1 ? 0x88 : 0x89);
        emit_modrm(assembler, 3, source->reg, destination->reg);
        return true;
    }

    if (destination->kind == OPERAND_REG && source->kind == OPERAND_IMM) {
        int width = destination->width;
        if (width != 1 && width != 2 && width != 4 && width != 8) {
            return assembler_error(assembler, "invalid mov immediate width");
        }
        if (width == 2) {
            code_u8(&assembler->code, 0x66);
        }
        emit_rex(assembler, width == 8, 0, destination->reg, false);
        code_u8(&assembler->code, (uint8_t)(0xB8 + (destination->reg & 7)));
        if (width == 1) {
            code_u8(&assembler->code, (uint8_t)source->immediate);
        } else if (width == 2) {
            code_u16(&assembler->code, (uint16_t)source->immediate);
        } else if (width == 4) {
            code_u32(&assembler->code, (uint32_t)source->immediate);
        } else {
            code_u64(&assembler->code, source->immediate);
        }
        return true;
    }

    if (destination->kind == OPERAND_REG && source->kind == OPERAND_MEMORY) {
        int width = destination->width;
        if (source->memory_size != 0 && source->memory_size != width) {
            return assembler_error(assembler, "mov memory width mismatch");
        }
        if (width == 2) {
            code_u8(&assembler->code, 0x66);
        }
        emit_rex(assembler, width == 8, destination->reg, source->base,
                 width == 1 && need_rex_for_byte_register(destination->reg));
        code_u8(&assembler->code, width == 1 ? 0x8A : 0x8B);
        emit_memory(assembler, destination->reg, source);
        return true;
    }

    if (destination->kind == OPERAND_MEMORY && source->kind == OPERAND_REG) {
        int width = source->width;
        if (destination->memory_size != 0 &&
            destination->memory_size != width) {
            return assembler_error(assembler, "mov memory width mismatch");
        }
        if (width == 2) {
            code_u8(&assembler->code, 0x66);
        }
        emit_rex(assembler, width == 8, source->reg, destination->base,
                 width == 1 && need_rex_for_byte_register(source->reg));
        code_u8(&assembler->code, width == 1 ? 0x88 : 0x89);
        emit_memory(assembler, source->reg, destination);
        return true;
    }

    return assembler_error(assembler, "unsupported mov form");
}

static bool encode_movzx(CoffAssembler *assembler, const AsmOperand *ops,
                         int count) {
    if (!encode_expected(assembler, "movzx", count, 2)) {
        return false;
    }
    int width = ops[1].width;
    if (ops[0].kind != OPERAND_REG || ops[0].width != 4 ||
        (width != 1 && width != 2)) {
        return assembler_error(assembler, "unsupported movzx form");
    }
    if (ops[1].kind == OPERAND_MEMORY && ops[1].memory_size != width) {
        return assembler_error(assembler, "movzx memory width mismatch");
    }
    emit_rex(assembler, false, ops[0].reg,
             ops[1].kind == OPERAND_MEMORY ? ops[1].base : ops[1].reg,
             width == 1 && ops[1].kind == OPERAND_REG &&
                 need_rex_for_byte_register(ops[1].reg));
    code_u8(&assembler->code, 0x0F);
    code_u8(&assembler->code, width == 1 ? 0xB6 : 0xB7);
    if (ops[1].kind == OPERAND_MEMORY) {
        emit_memory(assembler, ops[0].reg, &ops[1]);
    } else {
        emit_modrm(assembler, 3, ops[0].reg, ops[1].reg);
    }
    return true;
}

static bool encode_movsx(CoffAssembler *assembler, const AsmOperand *ops,
                         int count, bool to_64) {
    if (!encode_expected(assembler, to_64 ? "movsx" : "movsxd", count, 2)) {
        return false;
    }
    int width = ops[1].width;
    if (ops[0].kind != OPERAND_REG || ops[0].width != 8) {
        return assembler_error(assembler, "unsupported movsx form");
    }
    if (to_64 && width != 1 && width != 2) {
        return assembler_error(assembler, "unsupported movsx source width");
    }
    if (!to_64 && width != 4) {
        return assembler_error(assembler, "unsupported movsxd source width");
    }
    if (ops[1].kind == OPERAND_MEMORY && ops[1].memory_size != width) {
        return assembler_error(assembler, "movsx memory width mismatch");
    }
    emit_rex(assembler, true, ops[0].reg,
             ops[1].kind == OPERAND_MEMORY ? ops[1].base : ops[1].reg,
             width == 1 && ops[1].kind == OPERAND_REG &&
                 need_rex_for_byte_register(ops[1].reg));
    if (to_64) {
        code_u8(&assembler->code, 0x0F);
        code_u8(&assembler->code, width == 1 ? 0xBE : 0xBF);
    } else {
        code_u8(&assembler->code, 0x63);
    }
    if (ops[1].kind == OPERAND_MEMORY) {
        emit_memory(assembler, ops[0].reg, &ops[1]);
    } else {
        emit_modrm(assembler, 3, ops[0].reg, ops[1].reg);
    }
    return true;
}

static uint8_t alu_opcode(const char *mnemonic) {
    if (strcmp(mnemonic, "add") == 0) return 0x01;
    if (strcmp(mnemonic, "sub") == 0) return 0x29;
    if (strcmp(mnemonic, "and") == 0) return 0x21;
    if (strcmp(mnemonic, "or") == 0) return 0x09;
    if (strcmp(mnemonic, "xor") == 0) return 0x31;
    if (strcmp(mnemonic, "cmp") == 0) return 0x39;
    return 0;
}

static uint8_t alu_digit(const char *mnemonic) {
    if (strcmp(mnemonic, "add") == 0) return 0;
    if (strcmp(mnemonic, "or") == 0) return 1;
    if (strcmp(mnemonic, "and") == 0) return 4;
    if (strcmp(mnemonic, "sub") == 0) return 5;
    if (strcmp(mnemonic, "xor") == 0) return 6;
    if (strcmp(mnemonic, "cmp") == 0) return 7;
    return 0;
}

static bool encode_alu(CoffAssembler *assembler, const char *mnemonic,
                       const AsmOperand *ops, int count) {
    if (!encode_expected(assembler, mnemonic, count, 2)) {
        return false;
    }
    if (ops[0].kind != OPERAND_REG) {
        return assembler_error(assembler, "unsupported %s destination",
                               mnemonic);
    }
    int width = ops[0].width;

    if (ops[1].kind == OPERAND_REG) {
        if (ops[1].width != width) {
            return assembler_error(assembler, "%s register width mismatch",
                                   mnemonic);
        }
        if (width == 2) {
            code_u8(&assembler->code, 0x66);
        }
        emit_rex(assembler, width == 8, ops[1].reg, ops[0].reg,
                 width == 1 && (need_rex_for_byte_register(ops[0].reg) ||
                                need_rex_for_byte_register(ops[1].reg)));
        code_u8(&assembler->code, alu_opcode(mnemonic));
        emit_modrm(assembler, 3, ops[1].reg, ops[0].reg);
        return true;
    }

    if (ops[1].kind == OPERAND_IMM) {
        if (width == 2) {
            code_u8(&assembler->code, 0x66);
        }
        emit_rex(assembler, width == 8, 0, ops[0].reg, false);
        code_u8(&assembler->code, width == 1 ? 0x80 : 0x81);
        emit_modrm(assembler, 3, alu_digit(mnemonic), ops[0].reg);
        if (width == 1) {
            code_u8(&assembler->code, (uint8_t)ops[1].immediate);
        } else {
            code_u32(&assembler->code, (uint32_t)ops[1].immediate);
        }
        return true;
    }

    return assembler_error(assembler, "unsupported %s source", mnemonic);
}

static bool encode_test(CoffAssembler *assembler, const AsmOperand *ops,
                        int count) {
    if (!encode_expected(assembler, "test", count, 2)) {
        return false;
    }
    if (ops[0].kind != OPERAND_REG || ops[1].kind != OPERAND_REG ||
        ops[0].width != ops[1].width) {
        return assembler_error(assembler, "unsupported test form");
    }
    int width = ops[0].width;
    if (width == 2) {
        code_u8(&assembler->code, 0x66);
    }
    emit_rex(assembler, width == 8, ops[1].reg, ops[0].reg,
             width == 1 && (need_rex_for_byte_register(ops[0].reg) ||
                            need_rex_for_byte_register(ops[1].reg)));
    code_u8(&assembler->code, width == 1 ? 0x84 : 0x85);
    emit_modrm(assembler, 3, ops[1].reg, ops[0].reg);
    return true;
}

static bool encode_shift(CoffAssembler *assembler, const char *mnemonic,
                         const AsmOperand *ops, int count) {
    if (!encode_expected(assembler, mnemonic, count, 2)) {
        return false;
    }
    if (ops[0].kind != OPERAND_REG || ops[1].kind != OPERAND_REG ||
        ops[1].reg != 1 || ops[1].width != 1) {
        return assembler_error(assembler, "unsupported shift form");
    }
    int width = ops[0].width;
    uint8_t digit = strcmp(mnemonic, "shl") == 0
                        ? 4
                        : strcmp(mnemonic, "shr") == 0 ? 5 : 7;
    if (width == 2) {
        code_u8(&assembler->code, 0x66);
    }
    emit_rex(assembler, width == 8, 0, ops[0].reg,
             width == 1 && need_rex_for_byte_register(ops[0].reg));
    code_u8(&assembler->code, width == 1 ? 0xD2 : 0xD3);
    emit_modrm(assembler, 3, digit, ops[0].reg);
    return true;
}

static bool encode_setcc(CoffAssembler *assembler, const char *mnemonic,
                         const AsmOperand *ops, int count) {
    static const struct {
        const char *name;
        uint8_t opcode;
    } kSetcc[] = {
        {"sete", 0x94},  {"setne", 0x95}, {"setl", 0x9C},  {"setle", 0x9E},
        {"setg", 0x9F},  {"setge", 0x9D}, {"setb", 0x92},  {"setbe", 0x96},
        {"seta", 0x97},  {"setae", 0x93},
    };
    if (!encode_expected(assembler, mnemonic, count, 1)) {
        return false;
    }
    if (ops[0].kind != OPERAND_REG || ops[0].width != 1) {
        return assembler_error(assembler, "unsupported setcc form");
    }
    uint8_t opcode = 0;
    for (size_t i = 0; i < sizeof(kSetcc) / sizeof(kSetcc[0]); ++i) {
        if (strcmp(kSetcc[i].name, mnemonic) == 0) {
            opcode = kSetcc[i].opcode;
            break;
        }
    }
    if (opcode == 0) {
        return assembler_error(assembler, "unknown setcc '%s'", mnemonic);
    }
    emit_rex(assembler, false, 0, ops[0].reg,
             need_rex_for_byte_register(ops[0].reg));
    code_u8(&assembler->code, 0x0F);
    code_u8(&assembler->code, opcode);
    emit_modrm(assembler, 3, 0, ops[0].reg);
    return true;
}

static bool encode_cmovne(CoffAssembler *assembler, const AsmOperand *ops,
                          int count) {
    if (!encode_expected(assembler, "cmovne", count, 2)) {
        return false;
    }
    if (ops[0].kind != OPERAND_REG || ops[1].kind != OPERAND_REG ||
        ops[0].width != 8 || ops[1].width != 8) {
        return assembler_error(assembler, "unsupported cmovne form");
    }
    emit_rex(assembler, true, ops[0].reg, ops[1].reg, false);
    code_u8(&assembler->code, 0x0F);
    code_u8(&assembler->code, 0x45);
    emit_modrm(assembler, 3, ops[0].reg, ops[1].reg);
    return true;
}

static bool encode_lea(CoffAssembler *assembler, const AsmOperand *ops,
                       int count) {
    if (!encode_expected(assembler, "lea", count, 2)) {
        return false;
    }
    if (ops[0].kind != OPERAND_REG || ops[0].width != 8 ||
        ops[1].kind != OPERAND_MEMORY) {
        return assembler_error(assembler, "unsupported lea form");
    }
    emit_rex(assembler, true, ops[0].reg, ops[1].base, false);
    code_u8(&assembler->code, 0x8D);
    emit_memory(assembler, ops[0].reg, &ops[1]);
    return true;
}

static bool encode_push_pop(CoffAssembler *assembler, const char *mnemonic,
                            const AsmOperand *ops, int count) {
    if (!encode_expected(assembler, mnemonic, count, 1)) {
        return false;
    }
    if (ops[0].kind != OPERAND_REG || ops[0].width != 8) {
        return assembler_error(assembler, "unsupported %s form", mnemonic);
    }
    emit_rex(assembler, false, 0, ops[0].reg, false);
    code_u8(&assembler->code,
            (uint8_t)((strcmp(mnemonic, "push") == 0 ? 0x50 : 0x58) +
                      (ops[0].reg & 7)));
    return true;
}

static void add_fixup(CoffAssembler *assembler, const char *name, bool is_call) {
    CoffFixup fixup;
    memset(&fixup, 0, sizeof(fixup));
    fixup.offset = (uint32_t)assembler->code.length;
    fixup.name = arena_dup(assembler, name);
    fixup.is_call = is_call;
    whippet_vec_push(assembler->arena, (void **)&assembler->fixups,
                     &assembler->fixup_count, &assembler->fixup_capacity,
                     sizeof(CoffFixup), &fixup);
}

static bool encode_jump(CoffAssembler *assembler, const char *mnemonic,
                        const AsmOperand *ops, int count) {
    if (!encode_expected(assembler, mnemonic, count, 1)) {
        return false;
    }
    if (ops[0].kind != OPERAND_NAME) {
        return assembler_error(assembler, "%s expects a label", mnemonic);
    }
    if (strcmp(mnemonic, "jmp") == 0) {
        code_u8(&assembler->code, 0xE9);
    } else if (strcmp(mnemonic, "je") == 0) {
        code_u8(&assembler->code, 0x0F);
        code_u8(&assembler->code, 0x84);
    } else {
        code_u8(&assembler->code, 0x0F);
        code_u8(&assembler->code, 0x85);
    }
    add_fixup(assembler, ops[0].name, false);
    code_u32(&assembler->code, 0);
    return true;
}

static bool encode_call(CoffAssembler *assembler, const AsmOperand *ops,
                        int count) {
    if (!encode_expected(assembler, "call", count, 1)) {
        return false;
    }
    if (ops[0].kind == OPERAND_REG) {
        if (ops[0].width != 8) {
            return assembler_error(assembler, "unsupported call register");
        }
        emit_rex(assembler, false, 0, ops[0].reg, false);
        code_u8(&assembler->code, 0xFF);
        emit_modrm(assembler, 3, 2, ops[0].reg);
        return true;
    }
    if (ops[0].kind != OPERAND_NAME) {
        return assembler_error(assembler, "unsupported call target");
    }
    code_u8(&assembler->code, 0xE8);
    add_fixup(assembler, ops[0].name, true);
    code_u32(&assembler->code, 0);
    return true;
}

static bool encode_instruction(CoffAssembler *assembler, char *mnemonic,
                               AsmOperand *ops, int count) {
    if (strcmp(mnemonic, "mov") == 0) {
        return encode_mov(assembler, ops, count);
    }
    if (strcmp(mnemonic, "movzx") == 0) {
        return encode_movzx(assembler, ops, count);
    }
    if (strcmp(mnemonic, "movsx") == 0) {
        return encode_movsx(assembler, ops, count, true);
    }
    if (strcmp(mnemonic, "movsxd") == 0) {
        return encode_movsx(assembler, ops, count, false);
    }
    if (strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "sub") == 0 ||
        strcmp(mnemonic, "and") == 0 || strcmp(mnemonic, "or") == 0 ||
        strcmp(mnemonic, "xor") == 0 || strcmp(mnemonic, "cmp") == 0) {
        return encode_alu(assembler, mnemonic, ops, count);
    }
    if (strcmp(mnemonic, "test") == 0) {
        return encode_test(assembler, ops, count);
    }
    if (strcmp(mnemonic, "shl") == 0 || strcmp(mnemonic, "shr") == 0 ||
        strcmp(mnemonic, "sar") == 0) {
        return encode_shift(assembler, mnemonic, ops, count);
    }
    if (strncmp(mnemonic, "set", 3) == 0) {
        return encode_setcc(assembler, mnemonic, ops, count);
    }
    if (strcmp(mnemonic, "cmovne") == 0) {
        return encode_cmovne(assembler, ops, count);
    }
    if (strcmp(mnemonic, "lea") == 0) {
        return encode_lea(assembler, ops, count);
    }
    if (strcmp(mnemonic, "push") == 0 || strcmp(mnemonic, "pop") == 0) {
        return encode_push_pop(assembler, mnemonic, ops, count);
    }
    if (strcmp(mnemonic, "jmp") == 0 || strcmp(mnemonic, "je") == 0 ||
        strcmp(mnemonic, "jne") == 0) {
        return encode_jump(assembler, mnemonic, ops, count);
    }
    if (strcmp(mnemonic, "call") == 0) {
        return encode_call(assembler, ops, count);
    }
    if (strcmp(mnemonic, "ret") == 0) {
        if (!encode_expected(assembler, "ret", count, 0)) {
            return false;
        }
        code_u8(&assembler->code, 0xC3);
        return true;
    }
    if (strcmp(mnemonic, "cqo") == 0) {
        if (!encode_expected(assembler, "cqo", count, 0)) {
            return false;
        }
        emit_rex(assembler, true, 0, 0, false);
        code_u8(&assembler->code, 0x99);
        return true;
    }
    if (strcmp(mnemonic, "imul") == 0) {
        if (!encode_expected(assembler, "imul", count, 2) ||
            ops[0].kind != OPERAND_REG || ops[1].kind != OPERAND_REG ||
            ops[0].width != 8 || ops[1].width != 8) {
            return assembler_error(assembler, "unsupported imul form");
        }
        emit_rex(assembler, true, ops[0].reg, ops[1].reg, false);
        code_u8(&assembler->code, 0x0F);
        code_u8(&assembler->code, 0xAF);
        emit_modrm(assembler, 3, ops[0].reg, ops[1].reg);
        return true;
    }
    if (strcmp(mnemonic, "idiv") == 0 || strcmp(mnemonic, "div") == 0) {
        if (!encode_expected(assembler, mnemonic, count, 1) ||
            ops[0].kind != OPERAND_REG || ops[0].width != 8) {
            return assembler_error(assembler, "unsupported %s form", mnemonic);
        }
        emit_rex(assembler, true, 0, ops[0].reg, false);
        code_u8(&assembler->code, 0xF7);
        emit_modrm(assembler, 3,
                   strcmp(mnemonic, "idiv") == 0 ? 7 : 6, ops[0].reg);
        return true;
    }
    if (strcmp(mnemonic, "neg") == 0 || strcmp(mnemonic, "not") == 0) {
        if (!encode_expected(assembler, mnemonic, count, 1) ||
            ops[0].kind != OPERAND_REG || ops[0].width != 8) {
            return assembler_error(assembler, "unsupported %s form", mnemonic);
        }
        emit_rex(assembler, true, 0, ops[0].reg, false);
        code_u8(&assembler->code, 0xF7);
        emit_modrm(assembler, 3,
                   strcmp(mnemonic, "neg") == 0 ? 3 : 2, ops[0].reg);
        return true;
    }
    return assembler_error(assembler, "unsupported instruction '%s'", mnemonic);
}

/* ------------------------------------------------------------------ */
/* Text parser                                                         */
/* ------------------------------------------------------------------ */

static CoffLabel *find_label(CoffAssembler *assembler, const char *name) {
    for (uint32_t i = 0; i < assembler->label_count; ++i) {
        if (strcmp(assembler->labels[i].name, name) == 0) {
            return &assembler->labels[i];
        }
    }
    return NULL;
}

static int find_symbol(CoffAssembler *assembler, const char *name) {
    for (uint32_t i = 0; i < assembler->symbol_count; ++i) {
        if (strcmp(assembler->symbols[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool is_global_name(CoffAssembler *assembler, const char *name) {
    for (uint32_t i = 0; i < assembler->global_count; ++i) {
        if (strcmp(assembler->globals[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void add_global_name(CoffAssembler *assembler, const char *name) {
    char *copy = arena_dup(assembler, name);
    whippet_vec_push(assembler->arena, (void **)&assembler->globals,
                     &assembler->global_count, &assembler->global_capacity,
                     sizeof(char *), &copy);
}

static int add_symbol(CoffAssembler *assembler, const char *name,
                      uint32_t value, int16_t section) {
    int existing = find_symbol(assembler, name);
    if (existing >= 0) {
        return existing;
    }
    CoffSymbol symbol;
    memset(&symbol, 0, sizeof(symbol));
    symbol.name = arena_dup(assembler, name);
    symbol.value = value;
    symbol.section = section;
    uint32_t index = assembler->symbol_count;
    whippet_vec_push(assembler->arena, (void **)&assembler->symbols,
                     &assembler->symbol_count, &assembler->symbol_capacity,
                     sizeof(CoffSymbol), &symbol);
    return (int)index;
}

static bool add_label(CoffAssembler *assembler, const char *name,
                      uint32_t offset) {
    if (find_label(assembler, name) != NULL) {
        return assembler_error(assembler, "duplicate label '%s'", name);
    }
    CoffLabel label;
    memset(&label, 0, sizeof(label));
    label.name = arena_dup(assembler, name);
    label.offset = offset;
    label.global = is_global_name(assembler, name);
    whippet_vec_push(assembler->arena, (void **)&assembler->labels,
                     &assembler->label_count, &assembler->label_capacity,
                     sizeof(CoffLabel), &label);
    if (label.global) {
        add_symbol(assembler, name, offset, 1);
    }
    return true;
}

static bool parse_instruction(CoffAssembler *assembler, char *line) {
    char *cursor = line;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        cursor += 1;
    }
    char *operands_text = "";
    if (*cursor != '\0') {
        *cursor = '\0';
        operands_text = trim_text(cursor + 1);
    }
    char *mnemonic = trim_text(line);

    char *parts[4];
    int count = 0;
    char *start = operands_text;
    int depth = 0;
    for (char *p = operands_text; *p != '\0'; ++p) {
        if (*p == '[') depth += 1;
        if (*p == ']') depth -= 1;
        if (*p == ',' && depth == 0) {
            *p = '\0';
            if (count >= 4) {
                return assembler_error(assembler, "too many operands");
            }
            parts[count++] = trim_text(start);
            start = p + 1;
        }
    }
    if (*trim_text(start) != '\0') {
        if (count >= 4) {
            return assembler_error(assembler, "too many operands");
        }
        parts[count++] = trim_text(start);
    }

    AsmOperand operands[3];
    memset(operands, 0, sizeof(operands));
    if (count > 3) {
        return assembler_error(assembler, "too many operands");
    }
    for (int i = 0; i < count; ++i) {
        if (!parse_operand(assembler, parts[i], &operands[i])) {
            return false;
        }
    }
    return encode_instruction(assembler, mnemonic, operands, count);
}

static bool resolve_fixups(CoffAssembler *assembler) {
    for (uint32_t i = 0; i < assembler->fixup_count; ++i) {
        CoffFixup *fixup = &assembler->fixups[i];
        CoffLabel *label = find_label(assembler, fixup->name);
        if (label != NULL) {
            int32_t displacement =
                (int32_t)(label->offset - (fixup->offset + 4u));
            memcpy(assembler->code.data + fixup->offset, &displacement, 4);
            continue;
        }
        if (!fixup->is_call) {
            return assembler_error(assembler, "undefined label '%s'",
                                   fixup->name);
        }
        int symbol_index = add_symbol(assembler, fixup->name, 0, 0);
        CoffReloc relocation;
        relocation.offset = fixup->offset;
        relocation.symbol_index = (uint32_t)symbol_index;
        whippet_vec_push(assembler->arena, (void **)&assembler->relocations,
                         &assembler->relocation_count,
                         &assembler->relocation_capacity, sizeof(CoffReloc),
                         &relocation);
    }
    return true;
}

static bool parse_assembly(CoffAssembler *assembler, const char *text,
                           size_t length) {
    char *copy = whippet_arena_strndup(assembler->arena, text, length);
    char *cursor = copy;
    char *end = copy + length;
    while (cursor < end) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline != NULL) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = end;
        }
        line = trim_text(line);
        if (*line == '\0' || *line == '#') {
            continue;
        }

        if (strncmp(line, ".globl", 6) == 0 &&
            (line[6] == ' ' || line[6] == '\t')) {
            char *name = trim_text(line + 6);
            if (*name != '\0') {
                add_global_name(assembler, name);
            }
            continue;
        }
        if (strncmp(line, ".intel_syntax", 13) == 0 ||
            strcmp(line, ".text") == 0) {
            continue;
        }

        size_t line_length = strlen(line);
        if (line_length != 0 && line[line_length - 1] == ':') {
            line[line_length - 1] = '\0';
            char *name = trim_text(line);
            if (*name == '\0') {
                return assembler_error(assembler, "empty label");
            }
            if (!add_label(assembler, name,
                           (uint32_t)assembler->code.length)) {
                return false;
            }
            continue;
        }

        if (line[0] == '.') {
            return assembler_error(assembler, "unsupported directive '%s'",
                                   line);
        }

        if (!parse_instruction(assembler, line)) {
            return false;
        }
    }
    return resolve_fixups(assembler);
}

/* ------------------------------------------------------------------ */
/* COFF writer                                                         */
/* ------------------------------------------------------------------ */

static size_t align4(size_t value) { return (value + 3u) & ~(size_t)3u; }

static void write_u16(FILE *file, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
    fwrite(bytes, 1, sizeof(bytes), file);
}

static void write_u32(FILE *file, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)((value >> 16) & 0xFF), (uint8_t)((value >> 24) & 0xFF),
    };
    fwrite(bytes, 1, sizeof(bytes), file);
}

typedef struct StringTable {
    char *data;
    size_t length;
    size_t capacity;
    WhippetArena *arena;
} StringTable;

static uint32_t string_table_add(StringTable *table, const char *name) {
    uint32_t offset = (uint32_t)table->length;
    size_t length = strlen(name) + 1u;
    if (table->length + length > table->capacity) {
        size_t capacity = table->capacity == 0 ? 256u : table->capacity;
        while (capacity < table->length + length) {
            capacity *= 2u;
        }
        char *fresh =
            (char *)whippet_arena_alloc(table->arena, capacity, 8);
        if (table->length != 0) {
            memcpy(fresh, table->data, table->length);
        }
        table->data = fresh;
        table->capacity = capacity;
    }
    memcpy(table->data + table->length, name, length);
    table->length += length;
    return offset;
}

static bool write_coff(CoffAssembler *assembler, FILE *out) {
    uint32_t code_size = (uint32_t)assembler->code.length;
    uint32_t relocation_count = assembler->relocation_count;
    uint32_t symbol_count = assembler->symbol_count;
    uint32_t raw_pointer = 60;
    uint32_t relocation_pointer =
        relocation_count == 0
            ? 0
            : (uint32_t)align4((size_t)raw_pointer + code_size);
    size_t after_relocations =
        relocation_count == 0
            ? (size_t)raw_pointer + code_size
            : (size_t)relocation_pointer + (size_t)relocation_count * 10u;
    uint32_t symbol_pointer =
        symbol_count == 0 ? 0 : (uint32_t)align4(after_relocations);

    uint32_t *name_offsets = (uint32_t *)whippet_arena_alloc(
        assembler->arena,
        (size_t)(symbol_count == 0 ? 1 : symbol_count) * sizeof(uint32_t), 8);
    memset(name_offsets, 0,
           (size_t)(symbol_count == 0 ? 1 : symbol_count) * sizeof(uint32_t));

    StringTable strings;
    memset(&strings, 0, sizeof(strings));
    strings.arena = assembler->arena;
    strings.capacity = 256;
    strings.data = (char *)whippet_arena_alloc(assembler->arena,
                                               strings.capacity, 8);
    memset(strings.data, 0, 4);
    strings.length = 4; /* COFF string table starts with a u32 size */
    for (uint32_t i = 0; i < symbol_count; ++i) {
        if (strlen(assembler->symbols[i].name) > 8) {
            name_offsets[i] = string_table_add(&strings,
                                               assembler->symbols[i].name);
        }
    }

    /* COFF file header. */
    write_u16(out, 0x8664);
    write_u16(out, 1);
    write_u32(out, 0);
    write_u32(out, symbol_pointer);
    write_u32(out, symbol_count);
    write_u16(out, 0);
    write_u16(out, 0);

    /* .text section header. */
    fwrite(".text\0\0\0", 1, 8, out);
    write_u32(out, 0);
    write_u32(out, 0);
    write_u32(out, code_size);
    write_u32(out, raw_pointer);
    write_u32(out, relocation_pointer);
    write_u32(out, 0);
    write_u16(out, (uint16_t)relocation_count);
    write_u16(out, 0);
    write_u32(out, 0x60000020u);

    if (code_size != 0) {
        fwrite(assembler->code.data, 1, code_size, out);
    }
    size_t position = raw_pointer + code_size;
    size_t relocations_start =
        relocation_pointer != 0 ? relocation_pointer : symbol_pointer;
    while (relocations_start != 0 && position < relocations_start) {
        fputc(0, out);
        position += 1;
    }

    for (uint32_t i = 0; i < relocation_count; ++i) {
        write_u32(out, assembler->relocations[i].offset);
        write_u32(out, assembler->relocations[i].symbol_index);
        write_u16(out, 0x0004); /* IMAGE_REL_AMD64_REL32 */
        position += 10;
    }
    while (symbol_pointer != 0 && position < symbol_pointer) {
        fputc(0, out);
        position += 1;
    }

    for (uint32_t i = 0; i < symbol_count; ++i) {
        const CoffSymbol *symbol = &assembler->symbols[i];
        size_t name_length = strlen(symbol->name);
        if (name_length <= 8) {
            char name[8] = {0};
            memcpy(name, symbol->name, name_length);
            fwrite(name, 1, 8, out);
        } else {
            write_u32(out, 0);
            write_u32(out, name_offsets[i]);
        }
        write_u32(out, symbol->value);
        write_u16(out, (uint16_t)symbol->section);
        write_u16(out, 0x20); /* function */
        uint8_t storage = 2;  /* external */
        uint8_t aux = 0;
        fwrite(&storage, 1, 1, out);
        fwrite(&aux, 1, 1, out);
    }

    if (symbol_count != 0) {
        uint32_t total = (uint32_t)strings.length;
        write_u32(out, total);
        if (strings.length > 4) {
            fwrite(strings.data + 4, 1, strings.length - 4, out);
        }
    }

    return ferror(out) == 0;
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

bool whippet_emit_object(const WhippetModule *module, FILE *out,
                         WhippetDiagnostic *diagnostic) {
    WhippetArena *arena = whippet_arena_create();
    WhippetText text;
    memset(&text, 0, sizeof(text));
    if (!whippet_codegen_module_asm_to_text(module, arena, &text, diagnostic)) {
        whippet_arena_destroy(arena);
        return false;
    }

    CoffAssembler assembler;
    memset(&assembler, 0, sizeof(assembler));
    assembler.arena = arena;
    assembler.code.arena = arena;
    assembler.diagnostic = diagnostic;

    if (!parse_assembly(&assembler, text.data, text.length)) {
        whippet_arena_destroy(arena);
        return false;
    }
    if (!write_coff(&assembler, out)) {
        if (diagnostic != NULL) {
            memset(diagnostic, 0, sizeof(*diagnostic));
            snprintf(diagnostic->message, sizeof(diagnostic->message),
                     "failed to write COFF object");
        }
        whippet_arena_destroy(arena);
        return false;
    }
    whippet_arena_destroy(arena);
    return true;
}
