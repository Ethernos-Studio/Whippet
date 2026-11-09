/*
 * Author: dhjs0000
 *
 * M1/M2 code generator: naive per-instruction selection for the integer
 * subset, GNU as (Intel syntax) text output, Windows x64 ABI.
 *
 * Strategy (design sections 6 and 8): correctness first, lots of redundant
 * moves. RAX/RCX/RDX are scratch registers; every computation is normalised
 * back to the IR type width (i8/i16/i32/i64/ptr). Phi results are stack
 * residents and each predecessor edge stores its incoming value into the phi
 * slot (edge blocks are split when necessary).
 */
#include "whippet/codegen.h"

#include "backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Assembly text buffer                                                */
/* ------------------------------------------------------------------ */

typedef struct AsmBuffer {
    WhippetText text;
    WhippetArena *arena;
} AsmBuffer;

static void asm_grow(AsmBuffer *buffer, size_t needed) {
    WhippetText *text = &buffer->text;
    if (text->length + needed + 1u <= text->capacity) {
        return;
    }
    size_t capacity = text->capacity == 0 ? 1024u : text->capacity;
    while (capacity < text->length + needed + 1u) {
        capacity *= 2u;
    }
    char *fresh =
        (char *)whippet_arena_alloc(buffer->arena, capacity, 8);
    if (text->length != 0) {
        memcpy(fresh, text->data, text->length);
    }
    text->data = fresh;
    text->capacity = capacity;
}

static void asm_append_n(AsmBuffer *buffer, const char *data, size_t length) {
    if (length == 0) {
        return;
    }
    asm_grow(buffer, length);
    memcpy(buffer->text.data + buffer->text.length, data, length);
    buffer->text.length += length;
    buffer->text.data[buffer->text.length] = '\0';
}

static void asm_append(AsmBuffer *buffer, const char *text) {
    asm_append_n(buffer, text, strlen(text));
}

static void asm_printf(AsmBuffer *buffer, const char *format, ...) {
    char stack_buffer[256];
    va_list arguments;
    va_start(arguments, format);
    int needed = vsnprintf(stack_buffer, sizeof(stack_buffer), format, arguments);
    va_end(arguments);
    if (needed < 0) {
        return;
    }
    if ((size_t)needed < sizeof(stack_buffer)) {
        asm_append_n(buffer, stack_buffer, (size_t)needed);
        return;
    }
    char *large =
        (char *)whippet_arena_alloc(buffer->arena, (size_t)needed + 1u, 1);
    va_start(arguments, format);
    vsnprintf(large, (size_t)needed + 1u, format, arguments);
    va_end(arguments);
    asm_append_n(buffer, large, (size_t)needed);
}

static bool codegen_error(WhippetDiagnostic *diagnostic, uint32_t location,
                          const char *format, ...) {
    if (diagnostic != NULL) {
        diagnostic->line =
            location == 0 ? 1u : WHIPPET_LOC_LINE(location);
        diagnostic->column =
            location == 0 ? 1u : WHIPPET_LOC_COLUMN(location);
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(diagnostic->message, sizeof(diagnostic->message), format,
                  arguments);
        va_end(arguments);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Registers and types                                                 */
/* ------------------------------------------------------------------ */

static const char *const kRegisterNames[MREG_COUNT][4] = {
    {"al", "ax", "eax", "rax"},       {"cl", "cx", "ecx", "rcx"},
    {"dl", "dx", "edx", "rdx"},       {"bl", "bx", "ebx", "rbx"},
    {"bpl", "bp", "ebp", "rbp"},      {"sil", "si", "esi", "rsi"},
    {"dil", "di", "edi", "rdi"},      {"r8b", "r8w", "r8d", "r8"},
    {"r9b", "r9w", "r9d", "r9"},      {"r10b", "r10w", "r10d", "r10"},
    {"r11b", "r11w", "r11d", "r11"},  {"r12b", "r12w", "r12d", "r12"},
    {"r13b", "r13w", "r13d", "r13"},  {"r14b", "r14w", "r14d", "r14"},
    {"r15b", "r15w", "r15d", "r15"},
};

static const char *reg_name(MReg reg, uint32_t size) {
    uint32_t index = size == 1u ? 0u : size == 2u ? 1u : size == 4u ? 2u : 3u;
    return kRegisterNames[reg][index];
}

static bool type_is_supported(WhippetType type) {
    return whippet_type_is_integer(type) || type == WHIPPET_TYPE_PTR;
}

static uint64_t canonical_constant(uint64_t value, WhippetType type) {
    switch (type) {
    case WHIPPET_TYPE_I8:
        return value & UINT64_C(0xFF);
    case WHIPPET_TYPE_I16:
        return value & UINT64_C(0xFFFF);
    case WHIPPET_TYPE_I32:
        return value & UINT64_C(0xFFFFFFFF);
    default:
        return value;
    }
}

/* ------------------------------------------------------------------ */
/* Diagnosis helpers                                                   */
/* ------------------------------------------------------------------ */

static const WhippetInstr *first_instruction(const WhippetModule *module,
                                             const WhippetBlock *block) {
    if (block->first == WHIPPET_HANDLE_NONE) {
        return NULL;
    }
    return whippet_module_node(module, block->first);
}

static bool block_has_phi(const WhippetModule *module,
                          const WhippetBlock *block) {
    const WhippetInstr *instruction = first_instruction(module, block);
    return instruction != NULL &&
           (WhippetOpcode)instruction->opcode == WHIPPET_OP_PHI;
}

static uint32_t count_phis(const WhippetModule *module,
                           const WhippetBlock *block) {
    uint32_t count = 0;
    const WhippetInstr *instruction = NULL;
    for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
         handle = instruction->next) {
        instruction = whippet_module_node(module, handle);
        if ((WhippetOpcode)instruction->opcode != WHIPPET_OP_PHI) {
            break;
        }
        count += 1u;
    }
    return count;
}

static uint32_t max_phis_per_block(const WhippetModule *module,
                                   const WhippetFunction *function) {
    uint32_t maximum = 0;
    for (const WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        uint32_t count = count_phis(module, block);
        if (count > maximum) {
            maximum = count;
        }
    }
    return maximum;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static bool unsupported_opcode(WhippetDiagnostic *diagnostic,
                               const WhippetInstr *instruction,
                               const char *milestone) {
    return codegen_error(
        diagnostic, instruction->location,
        "backend does not support '%s' yet (%s)",
        whippet_opcode_name((WhippetOpcode)instruction->opcode), milestone);
}

static bool validate_instruction(WhippetDiagnostic *diagnostic,
                                 const WhippetInstr *instruction) {
    WhippetOpcode opcode = (WhippetOpcode)instruction->opcode;

    switch (opcode) {
    case WHIPPET_OP_ADD:
    case WHIPPET_OP_SUB:
    case WHIPPET_OP_MUL:
    case WHIPPET_OP_SDIV:
    case WHIPPET_OP_UDIV:
    case WHIPPET_OP_SREM:
    case WHIPPET_OP_UREM:
    case WHIPPET_OP_AND:
    case WHIPPET_OP_OR:
    case WHIPPET_OP_XOR:
    case WHIPPET_OP_SHL:
    case WHIPPET_OP_LSHR:
    case WHIPPET_OP_ASHR:
    case WHIPPET_OP_NEG:
    case WHIPPET_OP_NOT:
    case WHIPPET_OP_ICMP:
    case WHIPPET_OP_PHI:
    case WHIPPET_OP_COPY:
    case WHIPPET_OP_SELECT:
    case WHIPPET_OP_TRUNC:
    case WHIPPET_OP_ZEXT:
    case WHIPPET_OP_SEXT:
    case WHIPPET_OP_BITCAST:
    case WHIPPET_OP_PTRTOINT:
    case WHIPPET_OP_INTTOPTR:
    case WHIPPET_OP_CALL:
    case WHIPPET_OP_ALLOCA:
    case WHIPPET_OP_LOAD:
    case WHIPPET_OP_STORE:
    case WHIPPET_OP_PTRADD:
    case WHIPPET_OP_RET:
    case WHIPPET_OP_BR:
    case WHIPPET_OP_CONDBR:
    case WHIPPET_OP_SWITCH:
        return true;

    case WHIPPET_OP_FADD:
    case WHIPPET_OP_FSUB:
    case WHIPPET_OP_FMUL:
    case WHIPPET_OP_FDIV:
    case WHIPPET_OP_FNEG:
    case WHIPPET_OP_FCMP:
    case WHIPPET_OP_FPEXT:
    case WHIPPET_OP_FPTRUNC:
    case WHIPPET_OP_SITOFP:
    case WHIPPET_OP_UITOFP:
    case WHIPPET_OP_FPTOSI:
    case WHIPPET_OP_FPTOUI:
        return unsupported_opcode(diagnostic, instruction,
                                  "planned for M4: floating point");

    default:
        return unsupported_opcode(diagnostic, instruction, "unknown opcode");
    }
}

static bool validate_function(MFunction *machine, WhippetDiagnostic *diagnostic) {
    WhippetFunction *function = machine->function;
    const WhippetModule *module = machine->module;

    for (uint32_t i = 0; i < function->param_count; ++i) {
        WhippetType type = (WhippetType)function->params[i].type;
        if (!type_is_supported(type)) {
            return codegen_error(
                diagnostic, function->location,
                "parameter '%s' has type '%s', which the M2 backend does not "
                "support",
                whippet_arena_string(module->arena,
                                     function->params[i].name),
                whippet_type_name(type));
        }
    }
    if (function->return_type != WHIPPET_TYPE_VOID &&
        !type_is_supported((WhippetType)function->return_type)) {
        return codegen_error(diagnostic, function->location,
                             "return type '%s' is not supported by the M2 "
                             "backend",
                             whippet_type_name(
                                 (WhippetType)function->return_type));
    }

    if (block_has_phi(module, function->first_block)) {
        return codegen_error(
            diagnostic, function->location,
            "phi in the entry block has no predecessor edge to store into");
    }

    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        const WhippetInstr *instruction = NULL;
        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(module, handle);
            if (!validate_instruction(diagnostic, instruction)) {
                return false;
            }
            WhippetOpcode opcode = (WhippetOpcode)instruction->opcode;
            const WhippetOperand *operands =
                whippet_instr_operands(module, instruction);
            if (opcode == WHIPPET_OP_ICMP &&
                !type_is_supported((WhippetType)operands[0].type)) {
                return codegen_error(
                    diagnostic, instruction->location,
                    "icmp on type '%s' is not supported by M2",
                    whippet_type_name((WhippetType)operands[0].type));
            }
            if (opcode == WHIPPET_OP_CONDBR &&
                !whippet_type_is_integer((WhippetType)operands[0].type)) {
                return codegen_error(
                    diagnostic, instruction->location,
                    "condbr condition must be an integer, got '%s'",
                    whippet_type_name((WhippetType)operands[0].type));
            }
            if (opcode == WHIPPET_OP_SWITCH &&
                !whippet_type_is_integer((WhippetType)operands[0].type)) {
                return codegen_error(
                    diagnostic, instruction->location,
                    "switch tag must be an integer, got '%s'",
                    whippet_type_name((WhippetType)operands[0].type));
            }
            if (instruction->result != 0) {
                WhippetType result_type =
                    (WhippetType)instruction->result_type;
                if (!type_is_supported(result_type)) {
                    return codegen_error(
                        diagnostic, instruction->location,
                        "result type '%s' is not supported by M2",
                        whippet_type_name(result_type));
                }
            }
            if (opcode >= WHIPPET_OP_TRUNC &&
                opcode <= WHIPPET_OP_INTTOPTR) {
                WhippetType source_type =
                    (WhippetType)instruction->aux;
                if (!type_is_supported(source_type)) {
                    return codegen_error(
                        diagnostic, instruction->location,
                        "cast source type '%s' is not supported by M2",
                        whippet_type_name(source_type));
                }
            }
            if (opcode == WHIPPET_OP_ALLOCA &&
                !type_is_supported((WhippetType)instruction->aux)) {
                return codegen_error(
                    diagnostic, instruction->location,
                    "alloca of type '%s' is not supported by M2",
                    whippet_type_name((WhippetType)instruction->aux));
            }
            if (opcode == WHIPPET_OP_STORE &&
                !type_is_supported((WhippetType)instruction->result_type)) {
                return codegen_error(
                    diagnostic, instruction->location,
                    "store of type '%s' is not supported by M2",
                    whippet_type_name(
                        (WhippetType)instruction->result_type));
            }
            if (opcode == WHIPPET_OP_CALL) {
                for (uint16_t i = 1; i < instruction->operand_count; ++i) {
                    if (!type_is_supported((WhippetType)operands[i].type)) {
                        return codegen_error(
                            diagnostic, instruction->location,
                            "call argument of type '%s' is not supported by M2",
                            whippet_type_name(
                                (WhippetType)operands[i].type));
                    }
                }
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Operand loading / storing                                           */
/* ------------------------------------------------------------------ */

static bool emit_load_operand(AsmBuffer *buffer, MFunction *machine,
                              const WhippetOperand *operand, MReg destination,
                              WhippetDiagnostic *diagnostic) {
    switch (operand->kind) {
    case WHIPPET_OPERAND_VALUE: {
        MLocation *location = &machine->locations[operand->resolved];
        if (location->kind == MLOC_REG) {
            if ((MReg)location->reg != destination) {
                asm_printf(buffer, "  mov %s, %s\n",
                           reg_name(destination, 8),
                           reg_name((MReg)location->reg, 8));
            }
            return true;
        }
        if (location->kind == MLOC_STACK) {
            asm_printf(buffer, "  mov %s, qword ptr [rsp + %d]\n",
                       reg_name(destination, 8), location->offset);
            return true;
        }
        return codegen_error(diagnostic, 0, "internal: value has no location");
    }
    case WHIPPET_OPERAND_INT: {
        uint64_t value =
            canonical_constant(operand->imm, (WhippetType)operand->type);
        asm_printf(buffer, "  mov %s, 0x%llx\n", reg_name(destination, 8),
                   (unsigned long long)value);
        return true;
    }
    case WHIPPET_OPERAND_FLOAT:
        return codegen_error(diagnostic, 0,
                             "floating-point constants are not supported by M2");
    default:
        return codegen_error(diagnostic, 0, "invalid operand for M2 codegen");
    }
}

static bool emit_store_rax(AsmBuffer *buffer, MFunction *machine,
                           uint32_t result, WhippetDiagnostic *diagnostic) {
    MLocation *location = &machine->locations[result];
    if (location->kind == MLOC_REG) {
        asm_printf(buffer, "  mov %s, rax\n",
                   reg_name((MReg)location->reg, 8));
        return true;
    }
    if (location->kind == MLOC_STACK) {
        asm_printf(buffer, "  mov qword ptr [rsp + %d], rax\n",
                   location->offset);
        return true;
    }
    return codegen_error(diagnostic, 0, "internal: result has no location");
}

static void emit_normalize_rax(AsmBuffer *buffer, WhippetType type) {
    switch (type) {
    case WHIPPET_TYPE_I8:
        asm_append(buffer, "  movzx eax, al\n");
        break;
    case WHIPPET_TYPE_I16:
        asm_append(buffer, "  movzx eax, ax\n");
        break;
    case WHIPPET_TYPE_I32:
        asm_append(buffer, "  mov eax, eax\n");
        break;
    default:
        break;
    }
}

static void emit_sign_extend(AsmBuffer *buffer, MReg reg, WhippetType type) {
    switch (type) {
    case WHIPPET_TYPE_I8:
        asm_printf(buffer, "  movsx %s, %s\n", reg_name(reg, 8),
                   reg_name(reg, 1));
        break;
    case WHIPPET_TYPE_I16:
        asm_printf(buffer, "  movsx %s, %s\n", reg_name(reg, 8),
                   reg_name(reg, 2));
        break;
    case WHIPPET_TYPE_I32:
        asm_printf(buffer, "  movsxd %s, %s\n", reg_name(reg, 8),
                   reg_name(reg, 4));
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Prologue / epilogue                                                 */
/* ------------------------------------------------------------------ */

static const MReg kSaveOrder[] = {
    MREG_RBX, MREG_RBP, MREG_RSI, MREG_RDI,
    MREG_R12, MREG_R13, MREG_R14, MREG_R15,
};

#define WHIPPET_SAVE_ORDER_COUNT \
    ((uint32_t)(sizeof(kSaveOrder) / sizeof(kSaveOrder[0])))

static const MReg kParameterRegisters[4] = {
    MREG_RCX,
    MREG_RDX,
    MREG_R8,
    MREG_R9,
};

static void emit_stage_parameter(AsmBuffer *buffer, MReg incoming,
                                 WhippetType type, uint32_t slot) {
    switch (type) {
    case WHIPPET_TYPE_I8:
        asm_printf(buffer, "  movzx eax, %s\n", reg_name(incoming, 1));
        break;
    case WHIPPET_TYPE_I16:
        asm_printf(buffer, "  movzx eax, %s\n", reg_name(incoming, 2));
        break;
    case WHIPPET_TYPE_I32:
        asm_printf(buffer, "  mov eax, %s\n", reg_name(incoming, 4));
        break;
    default:
        asm_printf(buffer, "  mov rax, %s\n", reg_name(incoming, 8));
        break;
    }
    asm_printf(buffer, "  mov qword ptr [rsp + %u], rax\n", slot * 8u);
}

static void emit_stage_stack_parameter(AsmBuffer *buffer, uint32_t offset,
                                       WhippetType type, uint32_t slot) {
    switch (type) {
    case WHIPPET_TYPE_I8:
        asm_printf(buffer, "  movzx eax, byte ptr [rsp + %u]\n", offset);
        break;
    case WHIPPET_TYPE_I16:
        asm_printf(buffer, "  movzx eax, word ptr [rsp + %u]\n", offset);
        break;
    case WHIPPET_TYPE_I32:
        asm_printf(buffer, "  mov eax, dword ptr [rsp + %u]\n", offset);
        break;
    default:
        asm_printf(buffer, "  mov rax, qword ptr [rsp + %u]\n", offset);
        break;
    }
    asm_printf(buffer, "  mov qword ptr [rsp + %u], rax\n", slot * 8u);
}

static uint32_t saved_register_count(const MFunction *machine) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < MREG_COUNT; ++i) {
        if (machine->saved_callee[i]) {
            count += 1u;
        }
    }
    return count;
}

static void emit_prologue(AsmBuffer *buffer, MFunction *machine) {
    uint32_t saved = saved_register_count(machine);
    for (uint32_t i = 0; i < WHIPPET_SAVE_ORDER_COUNT; ++i) {
        MReg reg = kSaveOrder[i];
        if (machine->saved_callee[reg]) {
            asm_printf(buffer, "  push %s\n", reg_name(reg, 8));
        }
    }
    if (machine->frame_size != 0) {
        asm_printf(buffer, "  sub rsp, %u\n", machine->frame_size);
    }

    WhippetFunction *function = machine->function;
    uint32_t param_base = machine->param_base_slot;
    for (uint32_t i = 0; i < function->param_count; ++i) {
        WhippetType type = (WhippetType)function->params[i].type;
        if (i < 4u) {
            emit_stage_parameter(buffer, kParameterRegisters[i], type,
                                 param_base + i);
        } else {
            /*
             * At callee entry the fifth argument is at [rsp+40]; the
             * prologue has since pushed saved registers and subtracted the
             * frame, so add both back.
             */
            uint32_t offset = 40u + saved * 8u + machine->frame_size +
                              8u * (i - 4u);
            emit_stage_stack_parameter(buffer, offset, type, param_base + i);
        }
    }
    for (uint32_t i = 0; i < function->param_count; ++i) {
        uint32_t value = function->params[i].value;
        MLocation *location = &machine->locations[value];
        if (location->kind == MLOC_REG) {
            uint32_t slot = param_base + i;
            asm_printf(buffer, "  mov %s, qword ptr [rsp + %u]\n",
                       reg_name((MReg)location->reg, 8), slot * 8u);
        }
    }
}

static void emit_epilogue(AsmBuffer *buffer, MFunction *machine) {
    if (machine->frame_size != 0) {
        asm_printf(buffer, "  add rsp, %u\n", machine->frame_size);
    }
    for (uint32_t i = WHIPPET_SAVE_ORDER_COUNT; i > 0; --i) {
        MReg reg = kSaveOrder[i - 1u];
        if (machine->saved_callee[reg]) {
            asm_printf(buffer, "  pop %s\n", reg_name(reg, 8));
        }
    }
    asm_append(buffer, "  ret\n");
}

/* ------------------------------------------------------------------ */
/* Labels and phi edges                                                */
/* ------------------------------------------------------------------ */

typedef struct EdgeMove {
    uint32_t phi_value;
    WhippetOperand operand;
} EdgeMove;

typedef struct Edge {
    uint32_t predecessor_id;
    uint32_t target_id;
    uint32_t move_start;
    uint32_t move_count;
} Edge;

typedef struct EdgeTable {
    WhippetArena *arena;
    Edge *edges;
    uint32_t edge_count;
    uint32_t edge_capacity;
    EdgeMove *moves;
    uint32_t move_count;
    uint32_t move_capacity;
} EdgeTable;

static void block_label(const MFunction *machine, uint32_t block_id, char *out,
                        size_t size) {
    snprintf(out, size, ".Lwhippet_%u_bb%u", machine->function->id, block_id);
}

static void edge_label(const MFunction *machine, uint32_t edge_index, char *out,
                       size_t size) {
    snprintf(out, size, ".Lwhippet_%u_edge%u", machine->function->id,
             edge_index);
}

static uint32_t find_edge(const EdgeTable *table, uint32_t predecessor_id,
                          uint32_t target_id) {
    for (uint32_t i = 0; i < table->edge_count; ++i) {
        if (table->edges[i].predecessor_id == predecessor_id &&
            table->edges[i].target_id == target_id) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool add_edge(EdgeTable *table, MFunction *machine,
                     uint32_t predecessor_id, uint32_t target_id,
                     WhippetDiagnostic *diagnostic) {
    WhippetFunction *function = machine->function;
    if (target_id == 0 || target_id > function->block_count) {
        return codegen_error(diagnostic, 0,
                             "internal: branch to invalid block id");
    }
    WhippetBlock *target = function->block_table[target_id - 1u];
    if (!block_has_phi(machine->module, target)) {
        return true;
    }
    if (find_edge(table, predecessor_id, target_id) != UINT32_MAX) {
        return true;
    }
    Edge edge;
    memset(&edge, 0, sizeof(edge));
    edge.predecessor_id = predecessor_id;
    edge.target_id = target_id;
    if (!whippet_vec_push(table->arena, (void **)&table->edges,
                          &table->edge_count, &table->edge_capacity,
                          sizeof(Edge), &edge)) {
        return codegen_error(diagnostic, 0, "out of memory in edge table");
    }
    return true;
}

static bool add_edge_move(EdgeTable *table, uint32_t phi_value,
                          const WhippetOperand *operand,
                          WhippetDiagnostic *diagnostic) {
    EdgeMove move;
    memset(&move, 0, sizeof(move));
    move.phi_value = phi_value;
    move.operand = *operand;
    if (!whippet_vec_push(table->arena, (void **)&table->moves,
                          &table->move_count, &table->move_capacity,
                          sizeof(EdgeMove), &move)) {
        return codegen_error(diagnostic, 0, "out of memory in edge moves");
    }
    return true;
}

static bool build_edges(EdgeTable *table, MFunction *machine,
                        WhippetDiagnostic *diagnostic) {
    WhippetFunction *function = machine->function;
    const WhippetModule *module = machine->module;

    /* Discover every edge whose target starts with phi instructions. */
    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        if (block->last == WHIPPET_HANDLE_NONE) {
            continue;
        }
        WhippetInstr *terminator = whippet_module_node(module, block->last);
        const WhippetOperand *operands =
            whippet_instr_operands(module, terminator);
        WhippetOpcode opcode = (WhippetOpcode)terminator->opcode;
        if (opcode == WHIPPET_OP_BR) {
            if (!add_edge(table, machine, block->id, operands[0].resolved,
                          diagnostic)) {
                return false;
            }
        } else if (opcode == WHIPPET_OP_CONDBR) {
            if (!add_edge(table, machine, block->id, operands[1].resolved,
                          diagnostic) ||
                !add_edge(table, machine, block->id, operands[2].resolved,
                          diagnostic)) {
                return false;
            }
        } else if (opcode == WHIPPET_OP_SWITCH) {
            for (uint16_t i = 1; i < terminator->operand_count; ++i) {
                if (!add_edge(table, machine, block->id,
                              operands[i].resolved, diagnostic)) {
                    return false;
                }
            }
        } else if (opcode != WHIPPET_OP_RET) {
            return codegen_error(diagnostic, terminator->location,
                                 "unsupported terminator in M2");
        }
    }

    /* Fill each edge with the phi moves for that predecessor. */
    for (uint32_t e = 0; e < table->edge_count; ++e) {
        Edge *edge = &table->edges[e];
        WhippetBlock *target = function->block_table[edge->target_id - 1u];
        const WhippetInstr *phi = NULL;
        edge->move_start = table->move_count;

        for (uint32_t handle = target->first; handle != WHIPPET_HANDLE_NONE;
             handle = phi->next) {
            phi = whippet_module_node(module, handle);
            if ((WhippetOpcode)phi->opcode != WHIPPET_OP_PHI) {
                break;
            }
            const WhippetOperand *operands =
                whippet_instr_operands(module, phi);
            bool found = false;
            for (uint16_t i = 0; i + 1u < phi->operand_count; i += 2u) {
                if (operands[i + 1u].kind == WHIPPET_OPERAND_BLOCK &&
                    operands[i + 1u].resolved == edge->predecessor_id) {
                    if (found) {
                        return codegen_error(
                            diagnostic, phi->location,
                            "phi has more than one incoming value for the "
                            "same predecessor");
                    }
                    if (operands[i].kind == WHIPPET_OPERAND_FLOAT) {
                        return codegen_error(
                            diagnostic, phi->location,
                            "floating-point phi values are not supported by M2");
                    }
                    if (!add_edge_move(table, phi->result, &operands[i],
                                       diagnostic)) {
                        return false;
                    }
                    found = true;
                }
            }
            if (!found) {
                WhippetBlock *predecessor =
                    function->block_table[edge->predecessor_id - 1u];
                return codegen_error(
                    diagnostic, phi->location,
                    "phi has no incoming value for predecessor '%s'",
                    whippet_arena_string(module->arena, predecessor->name));
            }
        }
        edge->move_count = table->move_count - edge->move_start;
        if (edge->move_count > machine->max_edge_moves) {
            return codegen_error(diagnostic, 0,
                                 "internal: phi edge scratch overflow");
        }
    }
    return true;
}

static bool branch_label(MFunction *machine, EdgeTable *table,
                         uint32_t predecessor_id, uint32_t target_id, char *out,
                         size_t size, WhippetDiagnostic *diagnostic,
                         uint32_t location) {
    WhippetFunction *function = machine->function;
    if (target_id == 0 || target_id > function->block_count) {
        return codegen_error(diagnostic, location,
                             "internal: branch to invalid block");
    }
    WhippetBlock *target = function->block_table[target_id - 1u];
    if (!block_has_phi(machine->module, target)) {
        block_label(machine, target_id, out, size);
        return true;
    }
    uint32_t edge = find_edge(table, predecessor_id, target_id);
    if (edge == UINT32_MAX) {
        return codegen_error(diagnostic, location,
                             "internal: missing phi edge block");
    }
    edge_label(machine, edge, out, size);
    return true;
}

static bool emit_edge_blocks(AsmBuffer *buffer, MFunction *machine,
                             EdgeTable *table,
                             WhippetDiagnostic *diagnostic) {
    for (uint32_t e = 0; e < table->edge_count; ++e) {
        Edge *edge = &table->edges[e];
        char edge_name[64];
        edge_label(machine, e, edge_name, sizeof(edge_name));
        asm_printf(buffer, "%s:\n", edge_name);

        /* Phase 1: snapshot every incoming value into edge scratch slots. */
        for (uint32_t i = 0; i < edge->move_count; ++i) {
            EdgeMove *move = &table->moves[edge->move_start + i];
            if (!emit_load_operand(buffer, machine, &move->operand, MREG_RAX,
                                   diagnostic)) {
                return false;
            }
            uint32_t scratch_slot = machine->scratch_base_slot + i;
            asm_printf(buffer, "  mov qword ptr [rsp + %u], rax\n",
                       scratch_slot * 8u);
        }

        /* Phase 2: write the snapshots into the phi result stack slots. */
        for (uint32_t i = 0; i < edge->move_count; ++i) {
            EdgeMove *move = &table->moves[edge->move_start + i];
            uint32_t scratch_slot = machine->scratch_base_slot + i;
            MLocation *destination =
                &machine->locations[move->phi_value];
            if (destination->kind != MLOC_STACK) {
                return codegen_error(diagnostic, 0,
                                     "internal: phi result is not stack "
                                     "resident");
            }
            asm_printf(buffer, "  mov rax, qword ptr [rsp + %u]\n",
                       scratch_slot * 8u);
            asm_printf(buffer, "  mov qword ptr [rsp + %d], rax\n",
                       destination->offset);
        }

        char target_name[64];
        block_label(machine, edge->target_id, target_name,
                    sizeof(target_name));
        asm_printf(buffer, "  jmp %s\n", target_name);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Instruction selection                                               */
/* ------------------------------------------------------------------ */

static bool emit_binary_int(AsmBuffer *buffer, MFunction *machine,
                            const WhippetInstr *instruction,
                            const char *mnemonic,
                            WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                           diagnostic) ||
        !emit_load_operand(buffer, machine, &operands[1], MREG_RCX,
                           diagnostic)) {
        return false;
    }
    asm_printf(buffer, "  %s rax, rcx\n", mnemonic);
    emit_normalize_rax(buffer, (WhippetType)instruction->result_type);
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static bool emit_division(AsmBuffer *buffer, MFunction *machine,
                          const WhippetInstr *instruction,
                          WhippetDiagnostic *diagnostic) {
    WhippetOpcode opcode = (WhippetOpcode)instruction->opcode;
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    WhippetType type = (WhippetType)instruction->result_type;
    bool is_signed = opcode == WHIPPET_OP_SDIV || opcode == WHIPPET_OP_SREM;
    bool is_remainder =
        opcode == WHIPPET_OP_SREM || opcode == WHIPPET_OP_UREM;

    if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                           diagnostic) ||
        !emit_load_operand(buffer, machine, &operands[1], MREG_RCX,
                           diagnostic)) {
        return false;
    }
    if (is_signed) {
        emit_sign_extend(buffer, MREG_RAX, type);
        emit_sign_extend(buffer, MREG_RCX, type);
        asm_append(buffer, "  cqo\n");
        asm_append(buffer, "  idiv rcx\n");
    } else {
        asm_append(buffer, "  xor edx, edx\n");
        asm_append(buffer, "  div rcx\n");
    }
    if (is_remainder) {
        asm_append(buffer, "  mov rax, rdx\n");
    }
    emit_normalize_rax(buffer, type);
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static bool emit_shift(AsmBuffer *buffer, MFunction *machine,
                       const WhippetInstr *instruction,
                       const char *mnemonic, WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    WhippetType type = (WhippetType)instruction->result_type;
    uint32_t size = whippet_type_size(type);

    if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                           diagnostic) ||
        !emit_load_operand(buffer, machine, &operands[1], MREG_RCX,
                           diagnostic)) {
        return false;
    }
    asm_printf(buffer, "  %s %s, cl\n", mnemonic, reg_name(MREG_RAX, size));
    emit_normalize_rax(buffer, type);
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static const char *const kICmpSetcc[10] = {
    "sete", "setne", "setl", "setle", "setg",
    "setge", "setb", "setbe", "seta", "setae",
};

static bool icmp_is_signed(uint8_t predicate) {
    return predicate >= WHIPPET_ICMP_SLT && predicate <= WHIPPET_ICMP_SGE;
}

static bool emit_icmp(AsmBuffer *buffer, MFunction *machine,
                      const WhippetInstr *instruction,
                      WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    WhippetType type = (WhippetType)operands[0].type;
    uint8_t predicate = instruction->aux;
    if (predicate >= sizeof(kICmpSetcc) / sizeof(kICmpSetcc[0])) {
        return codegen_error(diagnostic, instruction->location,
                             "invalid icmp predicate");
    }

    if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                           diagnostic) ||
        !emit_load_operand(buffer, machine, &operands[1], MREG_RCX,
                           diagnostic)) {
        return false;
    }
    if (type != WHIPPET_TYPE_PTR && icmp_is_signed(predicate)) {
        emit_sign_extend(buffer, MREG_RAX, type);
        emit_sign_extend(buffer, MREG_RCX, type);
    }
    asm_append(buffer, "  cmp rax, rcx\n");
    asm_printf(buffer, "  %s al\n", kICmpSetcc[predicate]);
    asm_append(buffer, "  movzx eax, al\n");
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static bool emit_select(AsmBuffer *buffer, MFunction *machine,
                        const WhippetInstr *instruction,
                        WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    if (!emit_load_operand(buffer, machine, &operands[2], MREG_RAX,
                           diagnostic) ||
        !emit_load_operand(buffer, machine, &operands[1], MREG_RDX,
                           diagnostic) ||
        !emit_load_operand(buffer, machine, &operands[0], MREG_RCX,
                           diagnostic)) {
        return false;
    }
    asm_append(buffer, "  test rcx, rcx\n");
    asm_append(buffer, "  cmovne rax, rdx\n");
    emit_normalize_rax(buffer, (WhippetType)instruction->result_type);
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static bool emit_cast(AsmBuffer *buffer, MFunction *machine,
                      const WhippetInstr *instruction,
                      WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    WhippetType source = (WhippetType)instruction->aux;
    WhippetType destination = (WhippetType)instruction->result_type;

    if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                           diagnostic)) {
        return false;
    }
    switch ((WhippetOpcode)instruction->opcode) {
    case WHIPPET_OP_TRUNC:
        emit_normalize_rax(buffer, destination);
        break;
    case WHIPPET_OP_ZEXT:
        emit_normalize_rax(buffer, destination);
        break;
    case WHIPPET_OP_SEXT:
        emit_sign_extend(buffer, MREG_RAX, source);
        emit_normalize_rax(buffer, destination);
        break;
    case WHIPPET_OP_BITCAST:
        break;
    case WHIPPET_OP_PTRTOINT:
        if (whippet_type_size(destination) < 8u) {
            emit_normalize_rax(buffer, destination);
        }
        break;
    case WHIPPET_OP_INTTOPTR:
        break;
    default:
        return codegen_error(diagnostic, instruction->location,
                             "unsupported M2 cast");
    }
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static bool emit_call(AsmBuffer *buffer, MFunction *machine,
                      const WhippetInstr *instruction,
                      WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    uint32_t argument_count = instruction->operand_count - 1u;

    /* Outgoing stack arguments live above the 32-byte shadow space. */
    for (uint32_t i = 4; i < argument_count; ++i) {
        if (!emit_load_operand(buffer, machine, &operands[i + 1u], MREG_RAX,
                               diagnostic)) {
            return false;
        }
        asm_printf(buffer, "  mov qword ptr [rsp + %u], rax\n",
                   32u + (i - 4u) * 8u);
    }

    uint32_t register_arguments = argument_count < 4u ? argument_count : 4u;
    for (uint32_t i = 0; i < register_arguments; ++i) {
        if (!emit_load_operand(buffer, machine, &operands[i + 1u],
                               kParameterRegisters[i], diagnostic)) {
            return false;
        }
    }

    if (operands[0].kind == WHIPPET_OPERAND_SYMBOL) {
        asm_printf(buffer, "  call %s\n",
                   whippet_arena_string(machine->module->arena,
                                        operands[0].name));
    } else {
        if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                               diagnostic)) {
            return false;
        }
        asm_append(buffer, "  call rax\n");
    }

    if (instruction->result != 0) {
        emit_normalize_rax(buffer, (WhippetType)instruction->result_type);
        return emit_store_rax(buffer, machine, instruction->result,
                              diagnostic);
    }
    return true;
}

static bool emit_alloca(AsmBuffer *buffer, MFunction *machine,
                        const WhippetInstr *instruction,
                        uint32_t instruction_handle,
                        WhippetDiagnostic *diagnostic) {
    int32_t offset = machine->alloca_offsets[instruction_handle];
    asm_printf(buffer, "  lea rax, [rsp + %d]\n", offset);
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static bool emit_load_memory(AsmBuffer *buffer, MFunction *machine,
                             const WhippetInstr *instruction,
                             WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    WhippetType type = (WhippetType)instruction->result_type;
    if (!emit_load_operand(buffer, machine, &operands[0], MREG_RCX,
                           diagnostic)) {
        return false;
    }
    switch (type) {
    case WHIPPET_TYPE_I8:
        asm_append(buffer, "  movzx eax, byte ptr [rcx]\n");
        break;
    case WHIPPET_TYPE_I16:
        asm_append(buffer, "  movzx eax, word ptr [rcx]\n");
        break;
    case WHIPPET_TYPE_I32:
        asm_append(buffer, "  mov eax, dword ptr [rcx]\n");
        break;
    case WHIPPET_TYPE_I64:
    case WHIPPET_TYPE_PTR:
        asm_append(buffer, "  mov rax, qword ptr [rcx]\n");
        break;
    default:
        return codegen_error(diagnostic, instruction->location,
                             "load of type '%s' is not supported",
                             whippet_type_name(type));
    }
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static bool emit_store_memory(AsmBuffer *buffer, MFunction *machine,
                              const WhippetInstr *instruction,
                              WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    WhippetType type = (WhippetType)instruction->result_type;
    if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                           diagnostic) ||
        !emit_load_operand(buffer, machine, &operands[1], MREG_RCX,
                           diagnostic)) {
        return false;
    }
    switch (type) {
    case WHIPPET_TYPE_I8:
        asm_append(buffer, "  mov byte ptr [rcx], al\n");
        break;
    case WHIPPET_TYPE_I16:
        asm_append(buffer, "  mov word ptr [rcx], ax\n");
        break;
    case WHIPPET_TYPE_I32:
        asm_append(buffer, "  mov dword ptr [rcx], eax\n");
        break;
    case WHIPPET_TYPE_I64:
    case WHIPPET_TYPE_PTR:
        asm_append(buffer, "  mov qword ptr [rcx], rax\n");
        break;
    default:
        return codegen_error(diagnostic, instruction->location,
                             "store of type '%s' is not supported",
                             whippet_type_name(type));
    }
    return true;
}

static bool emit_ptradd(AsmBuffer *buffer, MFunction *machine,
                        const WhippetInstr *instruction,
                        WhippetDiagnostic *diagnostic) {
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    WhippetType offset_type = (WhippetType)operands[1].type;
    if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                           diagnostic) ||
        !emit_load_operand(buffer, machine, &operands[1], MREG_RCX,
                           diagnostic)) {
        return false;
    }
    emit_sign_extend(buffer, MREG_RCX, offset_type);
    asm_append(buffer, "  add rax, rcx\n");
    return emit_store_rax(buffer, machine, instruction->result, diagnostic);
}

static bool emit_instruction(AsmBuffer *buffer, MFunction *machine,
                             WhippetBlock *block,
                             uint32_t instruction_handle,
                             const WhippetInstr *instruction, EdgeTable *edges,
                             WhippetDiagnostic *diagnostic) {
    WhippetOpcode opcode = (WhippetOpcode)instruction->opcode;
    const WhippetOperand *operands =
        whippet_instr_operands(machine->module, instruction);
    WhippetType result_type = (WhippetType)instruction->result_type;

    switch (opcode) {
    case WHIPPET_OP_ADD:
        return emit_binary_int(buffer, machine, instruction, "add", diagnostic);
    case WHIPPET_OP_SUB:
        return emit_binary_int(buffer, machine, instruction, "sub", diagnostic);
    case WHIPPET_OP_MUL:
        return emit_binary_int(buffer, machine, instruction, "imul",
                               diagnostic);
    case WHIPPET_OP_AND:
        return emit_binary_int(buffer, machine, instruction, "and", diagnostic);
    case WHIPPET_OP_OR:
        return emit_binary_int(buffer, machine, instruction, "or", diagnostic);
    case WHIPPET_OP_XOR:
        return emit_binary_int(buffer, machine, instruction, "xor", diagnostic);

    case WHIPPET_OP_SDIV:
    case WHIPPET_OP_UDIV:
    case WHIPPET_OP_SREM:
    case WHIPPET_OP_UREM:
        return emit_division(buffer, machine, instruction, diagnostic);

    case WHIPPET_OP_SHL:
        return emit_shift(buffer, machine, instruction, "shl", diagnostic);
    case WHIPPET_OP_LSHR:
        return emit_shift(buffer, machine, instruction, "shr", diagnostic);
    case WHIPPET_OP_ASHR:
        return emit_shift(buffer, machine, instruction, "sar", diagnostic);

    case WHIPPET_OP_NEG:
    case WHIPPET_OP_NOT:
        if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                               diagnostic)) {
            return false;
        }
        asm_append(buffer, opcode == WHIPPET_OP_NEG ? "  neg rax\n"
                                                    : "  not rax\n");
        emit_normalize_rax(buffer, result_type);
        return emit_store_rax(buffer, machine, instruction->result, diagnostic);

    case WHIPPET_OP_ICMP:
        return emit_icmp(buffer, machine, instruction, diagnostic);

    case WHIPPET_OP_PHI:
        /* Phi results are written by the predecessor edge blocks. */
        return true;

    case WHIPPET_OP_COPY:
        if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                               diagnostic)) {
            return false;
        }
        return emit_store_rax(buffer, machine, instruction->result, diagnostic);

    case WHIPPET_OP_SELECT:
        return emit_select(buffer, machine, instruction, diagnostic);

    case WHIPPET_OP_TRUNC:
    case WHIPPET_OP_ZEXT:
    case WHIPPET_OP_SEXT:
    case WHIPPET_OP_BITCAST:
    case WHIPPET_OP_PTRTOINT:
    case WHIPPET_OP_INTTOPTR:
        return emit_cast(buffer, machine, instruction, diagnostic);

    case WHIPPET_OP_CALL:
        return emit_call(buffer, machine, instruction, diagnostic);

    case WHIPPET_OP_ALLOCA:
        return emit_alloca(buffer, machine, instruction, instruction_handle,
                           diagnostic);

    case WHIPPET_OP_LOAD:
        return emit_load_memory(buffer, machine, instruction, diagnostic);

    case WHIPPET_OP_STORE:
        return emit_store_memory(buffer, machine, instruction, diagnostic);

    case WHIPPET_OP_PTRADD:
        return emit_ptradd(buffer, machine, instruction, diagnostic);

    case WHIPPET_OP_RET:
        if (instruction->operand_count == 1) {
            if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                                   diagnostic)) {
                return false;
            }
        }
        emit_epilogue(buffer, machine);
        return true;

    case WHIPPET_OP_BR: {
        char label[64];
        if (!branch_label(machine, edges, block->id, operands[0].resolved,
                          label, sizeof(label), diagnostic,
                          instruction->location)) {
            return false;
        }
        asm_printf(buffer, "  jmp %s\n", label);
        return true;
    }

    case WHIPPET_OP_CONDBR: {
        char true_label[64];
        char false_label[64];
        if (!branch_label(machine, edges, block->id, operands[1].resolved,
                          true_label, sizeof(true_label), diagnostic,
                          instruction->location) ||
            !branch_label(machine, edges, block->id, operands[2].resolved,
                          false_label, sizeof(false_label), diagnostic,
                          instruction->location)) {
            return false;
        }
        if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                               diagnostic)) {
            return false;
        }
        asm_append(buffer, "  test rax, rax\n");
        asm_printf(buffer, "  jne %s\n", true_label);
        asm_printf(buffer, "  jmp %s\n", false_label);
        return true;
    }

    case WHIPPET_OP_SWITCH: {
        /* M2 lowers switch to a compare chain; jump tables are a later pass. */
        if (!emit_load_operand(buffer, machine, &operands[0], MREG_RAX,
                               diagnostic)) {
            return false;
        }
        for (uint16_t i = 1; i + 1u < instruction->operand_count; ++i) {
            char case_label[64];
            if (!branch_label(machine, edges, block->id,
                              operands[i].resolved, case_label,
                              sizeof(case_label), diagnostic,
                              instruction->location)) {
                return false;
            }
            asm_printf(buffer, "  cmp rax, %u\n", (uint32_t)(i - 1u));
            asm_printf(buffer, "  je %s\n", case_label);
        }
        char default_label[64];
        if (!branch_label(machine, edges, block->id,
                          operands[instruction->operand_count - 1u].resolved,
                          default_label, sizeof(default_label), diagnostic,
                          instruction->location)) {
            return false;
        }
        asm_printf(buffer, "  jmp %s\n", default_label);
        return true;
    }

    default:
        return unsupported_opcode(diagnostic, instruction, "M2");
    }
}

/* ------------------------------------------------------------------ */
/* Function / module emission                                          */
/* ------------------------------------------------------------------ */

static bool prepare_function(MFunction *machine, const WhippetModule *module,
                             WhippetFunction *function, WhippetArena *scratch,
                             WhippetDiagnostic *diagnostic) {
    memset(machine, 0, sizeof(*machine));
    machine->module = module;
    machine->function = function;
    machine->scratch = scratch;
    machine->max_edge_moves = max_phis_per_block(module, function);

    if (!validate_function(machine, diagnostic)) {
        return false;
    }

    machine->has_calls = false;
    machine->max_stack_args = 0;
    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        const WhippetInstr *instruction = NULL;
        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(module, handle);
            if ((WhippetOpcode)instruction->opcode == WHIPPET_OP_CALL) {
                machine->has_calls = true;
                uint32_t argument_count =
                    instruction->operand_count - 1u;
                if (argument_count > 4u &&
                    argument_count - 4u > machine->max_stack_args) {
                    machine->max_stack_args = argument_count - 4u;
                }
            }
        }
    }
    machine->call_area_slots =
        machine->has_calls ? 4u + machine->max_stack_args : 0u;

    if (!whippet_regalloc_run(machine, diagnostic)) {
        return false;
    }
    uint32_t saved_registers = 0;
    for (uint32_t i = 0; i < MREG_COUNT; ++i) {
        if (machine->saved_callee[i]) {
            saved_registers += 1u;
        }
    }
    uint32_t stack_use = machine->frame_size + saved_registers * 8u;
    if (stack_use > 4096u) {
        return codegen_error(
            diagnostic, function->location,
            "stack use of %u bytes exceeds the backend limit of 4096 "
            "(stack probing is not implemented yet)",
            stack_use);
    }
    return true;
}

static bool emit_function(AsmBuffer *buffer, const WhippetModule *module,
                          WhippetFunction *function,
                          WhippetDiagnostic *diagnostic) {
    WhippetArena *scratch = whippet_arena_create();
    MFunction machine;
    if (!prepare_function(&machine, module, function, scratch, diagnostic)) {
        whippet_arena_destroy(scratch);
        return false;
    }

    EdgeTable edges;
    memset(&edges, 0, sizeof(edges));
    edges.arena = scratch;
    if (!build_edges(&edges, &machine, diagnostic)) {
        whippet_arena_destroy(scratch);
        return false;
    }

    const char *name = whippet_arena_string(module->arena, function->name);
    asm_printf(buffer, ".globl %s\n", name);
    asm_printf(buffer, "%s:\n", name);
    emit_prologue(buffer, &machine);

    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        char label[64];
        block_label(&machine, block->id, label, sizeof(label));
        asm_printf(buffer, "%s:\n", label);
        const WhippetInstr *instruction = NULL;
        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(module, handle);
            if (!emit_instruction(buffer, &machine, block, handle, instruction,
                                  &edges, diagnostic)) {
                whippet_arena_destroy(scratch);
                return false;
            }
        }
    }

    if (!emit_edge_blocks(buffer, &machine, &edges, diagnostic)) {
        whippet_arena_destroy(scratch);
        return false;
    }
    whippet_arena_destroy(scratch);
    return true;
}

bool whippet_codegen_module_asm_to_text(const WhippetModule *module,
                                        WhippetArena *arena,
                                        WhippetText *out,
                                        WhippetDiagnostic *diagnostic) {
    if (diagnostic != NULL) {
        memset(diagnostic, 0, sizeof(*diagnostic));
    }
    AsmBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.arena = arena;
    asm_append(&buffer, ".intel_syntax noprefix\n.text\n\n");

    bool first = true;
    for (WhippetFunction *function = module->first_function; function != NULL;
         function = function->next) {
        if (function->is_declaration) {
            continue;
        }
        if (!first) {
            asm_append(&buffer, "\n");
        }
        first = false;
        if (!emit_function(&buffer, module, function, diagnostic)) {
            return false;
        }
    }
    *out = buffer.text;
    return true;
}

bool whippet_codegen_module_asm(const WhippetModule *module, FILE *out,
                                WhippetDiagnostic *diagnostic) {
    WhippetArena *arena = whippet_arena_create();
    WhippetText text;
    memset(&text, 0, sizeof(text));
    bool ok = whippet_codegen_module_asm_to_text(module, arena, &text,
                                                 diagnostic);
    if (ok) {
        ok = fwrite(text.data, 1, text.length, out) == text.length;
    }
    if (!ok && diagnostic != NULL && diagnostic->message[0] == '\0') {
        diagnostic->line = 0;
        diagnostic->column = 0;
        snprintf(diagnostic->message, sizeof(diagnostic->message),
                 "failed to write assembly output");
    }
    whippet_arena_destroy(arena);
    return ok;
}
