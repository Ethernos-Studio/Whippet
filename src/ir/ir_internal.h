/*
 * Author: dhjs0000
 *
 * Internal IR representation shared by the M0 parser, printer and future
 * passes. Handles are 1-based u32 indices; 0 is the null handle.
 */
#ifndef WHIPPET_IR_INTERNAL_H
#define WHIPPET_IR_INTERNAL_H

#include "whippet/ir.h"

#define WHIPPET_HANDLE_NONE 0u

/* Source locations are packed into one u32: 20 bits line, 12 bits column. */
#define WHIPPET_LOC_PACK(line_value, column_value) \
    (((uint32_t)(line_value) << 12) | ((uint32_t)(column_value) & 0xFFFu))
#define WHIPPET_LOC_LINE(value) ((uint32_t)(value) >> 12)
#define WHIPPET_LOC_COLUMN(value) ((uint32_t)(value) & 0xFFFu)
#define WHIPPET_LOC_NONE 0u

/* Instruction node: exactly 24 bytes, per design section 9.2. */
typedef struct WhippetInstr {
    uint32_t next;          /* next instruction handle in the block */
    uint32_t result;        /* defining value id (1-based), 0 if none */
    uint32_t operands;      /* first index into the module operand pool */
    uint32_t location;      /* packed source location */
    uint16_t opcode;        /* WhippetOpcode */
    uint16_t operand_count;
    uint8_t result_type;    /* WhippetType */
    uint8_t aux;            /* predicate or cast source type */
    uint16_t flags;         /* reserved, must be zero */
} WhippetInstr;

typedef enum WhippetOperandKind {
    WHIPPET_OPERAND_NONE = 0,
    WHIPPET_OPERAND_VALUE,  /* %name */
    WHIPPET_OPERAND_INT,    /* integer literal */
    WHIPPET_OPERAND_FLOAT,  /* floating-point literal */
    WHIPPET_OPERAND_BLOCK,  /* basic block label */
    WHIPPET_OPERAND_SYMBOL  /* @function */
} WhippetOperandKind;

typedef struct WhippetOperand {
    uint64_t imm;      /* integer bits or f64 bits */
    uint32_t name;     /* interned string handle */
    uint32_t resolved; /* value id / block id / function id, 0 until bound */
    uint8_t kind;      /* WhippetOperandKind */
    uint8_t type;      /* WhippetType, WHIPPET_TYPE_VOID if untyped */
    uint16_t reserved;
} WhippetOperand;

typedef struct WhippetValue {
    uint32_t name; /* interned string handle */
    uint32_t def;  /* defining instruction handle, 0 for parameters */
    uint8_t type;  /* WhippetType */
    uint8_t reserved[3];
} WhippetValue;

typedef struct WhippetParam {
    uint32_t name;
    uint32_t value; /* value id (1-based) for definitions, 0 for declarations */
    uint32_t type;  /* WhippetType */
} WhippetParam;

typedef struct WhippetBlock {
    uint32_t name;  /* interned label */
    uint32_t first; /* first instruction handle, 0 if empty */
    uint32_t last;  /* last instruction handle */
    uint32_t id;    /* 1-based block id inside the function */
    struct WhippetBlock *next;
} WhippetBlock;

typedef struct WhippetFunction {
    uint32_t name; /* interned string handle */
    uint32_t id;   /* 1-based function id in the module */
    WhippetArena *arena;
    struct WhippetFunction *next;
    WhippetBlock *first_block;
    WhippetBlock *last_block;
    WhippetBlock **block_table;
    WhippetParam *params;
    WhippetValue *values;
    uint32_t param_count;
    uint32_t param_capacity;
    uint32_t value_count;
    uint32_t value_capacity;
    uint32_t block_count;
    uint32_t block_capacity;
    uint8_t return_type;   /* WhippetType */
    uint8_t is_declaration;
} WhippetFunction;

#define WHIPPET_NODE_BLOCK_SHIFT 11u
#define WHIPPET_NODE_BLOCK_SIZE (1u << WHIPPET_NODE_BLOCK_SHIFT)
#define WHIPPET_NODE_BLOCK_MASK (WHIPPET_NODE_BLOCK_SIZE - 1u)

typedef struct WhippetNodeBlock {
    WhippetInstr nodes[WHIPPET_NODE_BLOCK_SIZE];
} WhippetNodeBlock;

struct WhippetModule {
    WhippetArena *arena;
    WhippetFunction *first_function;
    WhippetFunction *last_function;
    uint32_t function_count;

    WhippetNodeBlock **node_blocks;
    uint32_t node_block_count;
    uint32_t node_block_capacity;
    uint32_t node_count;

    WhippetOperand *operands;
    uint32_t operand_count;
    uint32_t operand_capacity;
    uint32_t *operand_locations;
};

typedef enum WhippetOpcodeKind {
    WHIPPET_KIND_INVALID = 0,
    WHIPPET_KIND_TERMINATOR,
    WHIPPET_KIND_BINARY_INT,
    WHIPPET_KIND_UNARY_INT,
    WHIPPET_KIND_BINARY_FP,
    WHIPPET_KIND_UNARY_FP,
    WHIPPET_KIND_ICMP,
    WHIPPET_KIND_FCMP,
    WHIPPET_KIND_ALLOCA,
    WHIPPET_KIND_LOAD,
    WHIPPET_KIND_STORE,
    WHIPPET_KIND_PTRADD,
    WHIPPET_KIND_PHI,
    WHIPPET_KIND_COPY,
    WHIPPET_KIND_SELECT,
    WHIPPET_KIND_CALL,
    WHIPPET_KIND_CAST
} WhippetOpcodeKind;

WhippetOpcodeKind whippet_opcode_kind(WhippetOpcode opcode);

/* ---- module construction helpers (ir.c) ---- */
WhippetModule *whippet_module_create(WhippetArena *arena);
WhippetFunction *whippet_function_create(WhippetModule *module, uint32_t name,
                                         WhippetType return_type,
                                         bool is_declaration);
uint32_t whippet_node_alloc(WhippetModule *module, WhippetOpcode opcode,
                            uint32_t location);
uint32_t whippet_operand_push(WhippetModule *module,
                              const WhippetOperand *operand,
                              uint32_t location);
bool whippet_vec_push(WhippetArena *arena, void **data, uint32_t *count,
                      uint32_t *capacity, size_t element_size,
                      const void *element);
bool whippet_function_add_value(WhippetFunction *function, uint32_t name,
                                WhippetType type, uint32_t def,
                                uint32_t *out_value_id);
bool whippet_function_add_block(WhippetFunction *function, WhippetBlock *block);
bool whippet_function_add_param(WhippetFunction *function,
                                const WhippetParam *param);
uint32_t whippet_arena_string_length(const WhippetArena *arena,
                                     uint32_t handle);

/* ---- node / operand accessors ---- */
static inline WhippetInstr *whippet_module_node(const WhippetModule *module,
                                                uint32_t handle) {
    uint32_t index = handle - 1u;
    return &module->node_blocks[index >> WHIPPET_NODE_BLOCK_SHIFT]
                ->nodes[index & WHIPPET_NODE_BLOCK_MASK];
}

static inline const WhippetOperand *whippet_module_operand(
    const WhippetModule *module, uint32_t index) {
    return &module->operands[index];
}

static inline const WhippetOperand *whippet_instr_operands(
    const WhippetModule *module, const WhippetInstr *instruction) {
    if (instruction->operand_count == 0) {
        return NULL;
    }
    return &module->operands[instruction->operands];
}

static inline uint32_t whippet_instr_location(const WhippetInstr *instruction) {
    return instruction->location;
}

#endif /* WHIPPET_IR_INTERNAL_H */
