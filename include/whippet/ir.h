/*
 * Whippet IR -- public interface.
 *
 * The text form of Whippet IR is the stable external contract between a
 * frontend and Whippet (see design/WHIPPET_DESIGN.md, sections 3 and 4).
 * M0 implements parsing, validation and canonical printing of that text;
 * no machine code is produced yet.
 *
 * Round-trip rule: parsing a canonically printed module and printing it
 * again yields byte-identical text. The printer normalises spacing,
 * comments, numeric spellings and blank lines; everything with semantics
 * is preserved.
 */
#ifndef WHIPPET_IR_H
#define WHIPPET_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WhippetArena WhippetArena;
typedef struct WhippetModule WhippetModule;

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/*
 * The seven operand types allowed by the contract, plus void for function
 * results only. There is deliberately no bool type: comparisons yield i32
 * and conditional branches treat any non-zero integer as true.
 */
typedef enum WhippetType {
    WHIPPET_TYPE_VOID = 0,
    WHIPPET_TYPE_I8,
    WHIPPET_TYPE_I16,
    WHIPPET_TYPE_I32,
    WHIPPET_TYPE_I64,
    WHIPPET_TYPE_F32,
    WHIPPET_TYPE_F64,
    WHIPPET_TYPE_PTR,
    WHIPPET_TYPE_COUNT
} WhippetType;

const char *whippet_type_name(WhippetType type);
bool whippet_type_from_string(const char *text, size_t length, WhippetType *out);
bool whippet_type_is_integer(WhippetType type);
bool whippet_type_is_float(WhippetType type);
bool whippet_type_is_numeric(WhippetType type);
/* Size in bytes with the Windows x64 data model (ptr = 8, long = 4). */
uint32_t whippet_type_size(WhippetType type);

/* ------------------------------------------------------------------ */
/* Opcodes                                                             */
/* ------------------------------------------------------------------ */

typedef enum WhippetOpcode {
    WHIPPET_OP_INVALID = 0,

    /* terminators */
    WHIPPET_OP_RET,
    WHIPPET_OP_BR,
    WHIPPET_OP_CONDBR,
    WHIPPET_OP_SWITCH,

    /* integer arithmetic and bit operations */
    WHIPPET_OP_ADD,
    WHIPPET_OP_SUB,
    WHIPPET_OP_MUL,
    WHIPPET_OP_SDIV,
    WHIPPET_OP_UDIV,
    WHIPPET_OP_SREM,
    WHIPPET_OP_UREM,
    WHIPPET_OP_AND,
    WHIPPET_OP_OR,
    WHIPPET_OP_XOR,
    WHIPPET_OP_SHL,
    WHIPPET_OP_LSHR,
    WHIPPET_OP_ASHR,

    /* unary integer */
    WHIPPET_OP_NEG,
    WHIPPET_OP_NOT,

    /* floating point arithmetic */
    WHIPPET_OP_FADD,
    WHIPPET_OP_FSUB,
    WHIPPET_OP_FMUL,
    WHIPPET_OP_FDIV,
    WHIPPET_OP_FNEG,

    /* comparisons (both produce i32 0/1) */
    WHIPPET_OP_ICMP,
    WHIPPET_OP_FCMP,

    /* memory */
    WHIPPET_OP_ALLOCA,
    WHIPPET_OP_LOAD,
    WHIPPET_OP_STORE,
    WHIPPET_OP_PTRADD,

    /* SSA helpers */
    WHIPPET_OP_PHI,
    WHIPPET_OP_COPY,
    WHIPPET_OP_SELECT,

    WHIPPET_OP_CALL,

    /* conversions */
    WHIPPET_OP_TRUNC,
    WHIPPET_OP_ZEXT,
    WHIPPET_OP_SEXT,
    WHIPPET_OP_FPEXT,
    WHIPPET_OP_FPTRUNC,
    WHIPPET_OP_BITCAST,
    WHIPPET_OP_PTRTOINT,
    WHIPPET_OP_INTTOPTR,
    WHIPPET_OP_SITOFP,
    WHIPPET_OP_UITOFP,
    WHIPPET_OP_FPTOSI,
    WHIPPET_OP_FPTOUI,

    WHIPPET_OP_COUNT
} WhippetOpcode;

typedef enum WhippetICmpPredicate {
    WHIPPET_ICMP_EQ = 0,
    WHIPPET_ICMP_NE,
    WHIPPET_ICMP_SLT,
    WHIPPET_ICMP_SLE,
    WHIPPET_ICMP_SGT,
    WHIPPET_ICMP_SGE,
    WHIPPET_ICMP_ULT,
    WHIPPET_ICMP_ULE,
    WHIPPET_ICMP_UGT,
    WHIPPET_ICMP_UGE
} WhippetICmpPredicate;

typedef enum WhippetFCmpPredicate {
    WHIPPET_FCMP_OEQ = 0,
    WHIPPET_FCMP_ONE,
    WHIPPET_FCMP_OLT,
    WHIPPET_FCMP_OLE,
    WHIPPET_FCMP_OGT,
    WHIPPET_FCMP_OGE,
    WHIPPET_FCMP_ORD,
    WHIPPET_FCMP_UNO,
    WHIPPET_FCMP_UEQ,
    WHIPPET_FCMP_UNE,
    WHIPPET_FCMP_ULT,
    WHIPPET_FCMP_ULE,
    WHIPPET_FCMP_UGT,
    WHIPPET_FCMP_UGE
} WhippetFCmpPredicate;

const char *whippet_opcode_name(WhippetOpcode opcode);
bool whippet_opcode_from_string(const char *text, size_t length, WhippetOpcode *out);
bool whippet_opcode_has_result(WhippetOpcode opcode);
const char *whippet_predicate_name(WhippetOpcode comparison, uint8_t predicate);
bool whippet_predicate_from_string(WhippetOpcode comparison, const char *text,
                                   size_t length, uint8_t *out);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

typedef struct WhippetDiagnostic {
    uint32_t line;   /* 1-based */
    uint32_t column; /* 1-based */
    char message[192];
} WhippetDiagnostic;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

/*
 * All IR storage comes from an arena owned by the caller. Destroying the
 * arena frees the module, all nodes, names and the printer buffer.
 */
WhippetArena *whippet_arena_create(void);
void whippet_arena_destroy(WhippetArena *arena);
/* Alignment must be a power of two; 0 means 8. Never returns NULL. */
void *whippet_arena_alloc(WhippetArena *arena, size_t size, size_t alignment);
char *whippet_arena_strndup(WhippetArena *arena, const char *text, size_t length);
/* Interns a byte string and returns a 1-based handle (0 means empty). */
uint32_t whippet_arena_intern(WhippetArena *arena, const char *text, size_t length);
const char *whippet_arena_string(const WhippetArena *arena, uint32_t handle);

/* ------------------------------------------------------------------ */
/* Parse / print                                                       */
/* ------------------------------------------------------------------ */

/*
 * Parses a .wir module. On failure returns false and fills *diagnostic;
 * *module is left NULL. On success *module is non-NULL and usable until
 * the arena is destroyed.
 */
bool whippet_parse_buffer(WhippetArena *arena, const char *source, size_t length,
                          WhippetModule **module, WhippetDiagnostic *diagnostic);
bool whippet_parse_file(WhippetArena *arena, const char *path,
                        WhippetModule **module, WhippetDiagnostic *diagnostic);

/* Reads a whole file into arena memory and appends a NUL terminator. */
bool whippet_read_file(WhippetArena *arena, const char *path,
                       char **data, size_t *length);

/*
 * Canonical text output. The buffer is arena-allocated and NUL terminated.
 * whippet_print_module_to_file writes the exact same bytes.
 */
typedef struct WhippetText {
    char *data;
    size_t length;
    size_t capacity;
} WhippetText;

bool whippet_print_module(const WhippetModule *module, WhippetArena *arena,
                          WhippetText *out);
bool whippet_print_module_to_file(const WhippetModule *module, FILE *file);

/* ------------------------------------------------------------------ */
/* Introspection (read-only, for tests and later milestones)           */
/* ------------------------------------------------------------------ */

uint32_t whippet_module_function_count(const WhippetModule *module);

#ifdef __cplusplus
}
#endif

#endif /* WHIPPET_IR_H */
