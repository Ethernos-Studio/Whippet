/*
 * Author: dhjs0000
 *
 * M3 O1 pass pipeline: constant folding/propagation, copy/select propagation,
 * dead code elimination and strength reduction (multiply by a power of two).
 *
 * The passes mutate the parsed IR in place. Every replacement updates all
 * operand uses (including phi incoming values) before the defining instruction
 * is unlinked, so the module stays SSA-valid for the backend.
 */
#include "whippet/passes.h"

#include "ir/ir_internal.h"

#include <string.h>

static uint64_t type_mask(WhippetType type) {
    switch (type) {
    case WHIPPET_TYPE_I8:
        return UINT64_C(0xFF);
    case WHIPPET_TYPE_I16:
        return UINT64_C(0xFFFF);
    case WHIPPET_TYPE_I32:
        return UINT64_C(0xFFFFFFFF);
    case WHIPPET_TYPE_I64:
    case WHIPPET_TYPE_PTR:
        return UINT64_C(0xFFFFFFFFFFFFFFFF);
    default:
        return 0;
    }
}

static int type_bits(WhippetType type) {
    switch (type) {
    case WHIPPET_TYPE_I8:
        return 8;
    case WHIPPET_TYPE_I16:
        return 16;
    case WHIPPET_TYPE_I32:
        return 32;
    case WHIPPET_TYPE_I64:
    case WHIPPET_TYPE_PTR:
        return 64;
    default:
        return 0;
    }
}

static bool is_integer_type(WhippetType type) {
    return type == WHIPPET_TYPE_I8 || type == WHIPPET_TYPE_I16 ||
           type == WHIPPET_TYPE_I32 || type == WHIPPET_TYPE_I64;
}

static int64_t sign_extend_bits(uint64_t value, WhippetType type) {
    switch (type) {
    case WHIPPET_TYPE_I8:
        return (int64_t)(int8_t)(uint8_t)value;
    case WHIPPET_TYPE_I16:
        return (int64_t)(int16_t)(uint16_t)value;
    case WHIPPET_TYPE_I32:
        return (int64_t)(int32_t)(uint32_t)value;
    case WHIPPET_TYPE_I64:
        return (int64_t)value;
    default:
        return 0;
    }
}

static bool operand_is_int(const WhippetOperand *operand) {
    return operand->kind == WHIPPET_OPERAND_INT;
}

static WhippetOperand make_int_operand(WhippetType type, uint64_t value) {
    WhippetOperand operand;
    memset(&operand, 0, sizeof(operand));
    operand.kind = WHIPPET_OPERAND_INT;
    operand.type = (uint8_t)type;
    operand.imm = value;
    return operand;
}

static bool is_pure_opcode(WhippetOpcode opcode) {
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
    case WHIPPET_OP_FADD:
    case WHIPPET_OP_FSUB:
    case WHIPPET_OP_FMUL:
    case WHIPPET_OP_FDIV:
    case WHIPPET_OP_FNEG:
    case WHIPPET_OP_ICMP:
    case WHIPPET_OP_FCMP:
    case WHIPPET_OP_ALLOCA:
    case WHIPPET_OP_PTRADD:
    case WHIPPET_OP_PHI:
    case WHIPPET_OP_COPY:
    case WHIPPET_OP_SELECT:
    case WHIPPET_OP_TRUNC:
    case WHIPPET_OP_ZEXT:
    case WHIPPET_OP_SEXT:
    case WHIPPET_OP_FPEXT:
    case WHIPPET_OP_FPTRUNC:
    case WHIPPET_OP_BITCAST:
    case WHIPPET_OP_PTRTOINT:
    case WHIPPET_OP_INTTOPTR:
    case WHIPPET_OP_SITOFP:
    case WHIPPET_OP_UITOFP:
    case WHIPPET_OP_FPTOSI:
    case WHIPPET_OP_FPTOUI:
        return true;
    default:
        return false;
    }
}

static uint32_t *compute_uses(WhippetModule *module, WhippetFunction *function,
                              WhippetArena *scratch) {
    size_t count = (size_t)function->value_count + 1u;
    uint32_t *uses = (uint32_t *)whippet_arena_alloc(
        scratch, count * sizeof(uint32_t), 8);
    memset(uses, 0, count * sizeof(uint32_t));
    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        uint32_t handle = block->first;
        while (handle != WHIPPET_HANDLE_NONE) {
            WhippetInstr *instruction = whippet_module_node(module, handle);
            WhippetOperand *operands =
                instruction->operand_count == 0
                    ? NULL
                    : &module->operands[instruction->operands];
            for (uint16_t i = 0; i < instruction->operand_count; ++i) {
                if (operands[i].kind == WHIPPET_OPERAND_VALUE &&
                    operands[i].resolved != 0 &&
                    operands[i].resolved <= function->value_count) {
                    uses[operands[i].resolved] += 1u;
                }
            }
            handle = instruction->next;
        }
    }
    return uses;
}

static void replace_uses(WhippetModule *module, WhippetFunction *function,
                         uint32_t value_id, const WhippetOperand *replacement) {
    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        uint32_t handle = block->first;
        while (handle != WHIPPET_HANDLE_NONE) {
            WhippetInstr *instruction = whippet_module_node(module, handle);
            if (instruction->operand_count != 0) {
                WhippetOperand *operands =
                    &module->operands[instruction->operands];
                for (uint16_t i = 0; i < instruction->operand_count; ++i) {
                    if (operands[i].kind == WHIPPET_OPERAND_VALUE &&
                        operands[i].resolved == value_id) {
                        operands[i] = *replacement;
                    }
                }
            }
            handle = instruction->next;
        }
    }
}

static bool delete_instruction(WhippetModule *module,
                               WhippetFunction *function, uint32_t handle) {
    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        uint32_t previous = WHIPPET_HANDLE_NONE;
        uint32_t current = block->first;
        while (current != WHIPPET_HANDLE_NONE) {
            WhippetInstr *instruction = whippet_module_node(module, current);
            if (current == handle) {
                if (previous == WHIPPET_HANDLE_NONE) {
                    block->first = instruction->next;
                } else {
                    whippet_module_node(module, previous)->next =
                        instruction->next;
                }
                if (block->last == handle) {
                    block->last = previous;
                }
                if (instruction->result != 0 &&
                    instruction->result <= function->value_count) {
                    /* The value has no remaining uses; mark it dead so the
                       allocator does not reserve a register/stack slot. */
                    function->values[instruction->result - 1u].type =
                        WHIPPET_TYPE_VOID;
                    function->values[instruction->result - 1u].def = 0;
                }
                return true;
            }
            previous = current;
            current = instruction->next;
        }
    }
    return false;
}

static bool fold_icmp(const WhippetInstr *instruction,
                      const WhippetOperand *operands, WhippetOperand *out) {
    const WhippetOperand *left = &operands[0];
    const WhippetOperand *right = &operands[1];
    if (!operand_is_int(left) || !operand_is_int(right) ||
        !is_integer_type((WhippetType)left->type)) {
        return false;
    }
    WhippetType type = (WhippetType)left->type;
    uint64_t mask = type_mask(type);
    uint64_t a = left->imm & mask;
    uint64_t b = right->imm & mask;
    int64_t sa = sign_extend_bits(a, type);
    int64_t sb = sign_extend_bits(b, type);
    bool result = false;
    switch ((WhippetICmpPredicate)instruction->aux) {
    case WHIPPET_ICMP_EQ:
        result = a == b;
        break;
    case WHIPPET_ICMP_NE:
        result = a != b;
        break;
    case WHIPPET_ICMP_SLT:
        result = sa < sb;
        break;
    case WHIPPET_ICMP_SLE:
        result = sa <= sb;
        break;
    case WHIPPET_ICMP_SGT:
        result = sa > sb;
        break;
    case WHIPPET_ICMP_SGE:
        result = sa >= sb;
        break;
    case WHIPPET_ICMP_ULT:
        result = a < b;
        break;
    case WHIPPET_ICMP_ULE:
        result = a <= b;
        break;
    case WHIPPET_ICMP_UGT:
        result = a > b;
        break;
    case WHIPPET_ICMP_UGE:
        result = a >= b;
        break;
    default:
        return false;
    }
    *out = make_int_operand(WHIPPET_TYPE_I32, result ? 1u : 0u);
    return true;
}

static bool fold_instruction(const WhippetInstr *instruction,
                             const WhippetOperand *operands,
                             WhippetOperand *out) {
    WhippetOpcode opcode = (WhippetOpcode)instruction->opcode;

    if (opcode == WHIPPET_OP_COPY) {
        if (instruction->operand_count != 1) {
            return false;
        }
        if (operands[0].kind == WHIPPET_OPERAND_VALUE ||
            operands[0].kind == WHIPPET_OPERAND_INT ||
            operands[0].kind == WHIPPET_OPERAND_FLOAT) {
            *out = operands[0];
            return true;
        }
        return false;
    }

    if (opcode == WHIPPET_OP_SELECT) {
        if (instruction->operand_count != 3 ||
            !operand_is_int(&operands[0])) {
            return false;
        }
        *out = operands[0].imm != 0 ? operands[1] : operands[2];
        return out->kind == WHIPPET_OPERAND_VALUE ||
               out->kind == WHIPPET_OPERAND_INT ||
               out->kind == WHIPPET_OPERAND_FLOAT;
    }

    if (opcode == WHIPPET_OP_PHI) {
        /* Do not fold loop-carried phis: an incoming value may be defined in
           the back-edge block and does not dominate the phi's uses. */
        return false;
    }

    if (opcode == WHIPPET_OP_ICMP) {
        if (instruction->operand_count != 2) {
            return false;
        }
        return fold_icmp(instruction, operands, out);
    }

    if (opcode == WHIPPET_OP_TRUNC || opcode == WHIPPET_OP_ZEXT ||
        opcode == WHIPPET_OP_SEXT || opcode == WHIPPET_OP_BITCAST) {
        if (instruction->operand_count != 1 ||
            !operand_is_int(&operands[0]) ||
            !is_integer_type((WhippetType)instruction->result_type)) {
            return false;
        }
        WhippetType source_type = (WhippetType)operands[0].type;
        WhippetType result_type = (WhippetType)instruction->result_type;
        if (!is_integer_type(source_type)) {
            return false;
        }
        uint64_t value = operands[0].imm;
        if (opcode == WHIPPET_OP_TRUNC) {
            value &= type_mask(result_type);
        } else if (opcode == WHIPPET_OP_ZEXT) {
            value &= type_mask(source_type);
            value &= type_mask(result_type);
        } else if (opcode == WHIPPET_OP_SEXT) {
            value = (uint64_t)sign_extend_bits(value, source_type) &
                    type_mask(result_type);
        } else {
            if (type_bits(source_type) != type_bits(result_type)) {
                return false;
            }
            value &= type_mask(result_type);
        }
        *out = make_int_operand(result_type, value);
        return true;
    }

    if (opcode == WHIPPET_OP_NEG || opcode == WHIPPET_OP_NOT) {
        if (instruction->operand_count != 1 ||
            !operand_is_int(&operands[0]) ||
            !is_integer_type((WhippetType)instruction->result_type)) {
            return false;
        }
        WhippetType type = (WhippetType)instruction->result_type;
        uint64_t mask = type_mask(type);
        uint64_t value = operands[0].imm & mask;
        uint64_t result =
            opcode == WHIPPET_OP_NEG ? (uint64_t)(0u - value) : (~value);
        *out = make_int_operand(type, result & mask);
        return true;
    }

    WhippetType result_type = (WhippetType)instruction->result_type;
    if (!is_integer_type(result_type)) {
        return false;
    }

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
        break;
    default:
        return false;
    }
    if (instruction->operand_count != 2) {
        return false;
    }

    if (!operand_is_int(&operands[0]) || !operand_is_int(&operands[1])) {
        return false;
    }
    WhippetType operand_type = (WhippetType)operands[0].type;
    if (!is_integer_type(operand_type)) {
        return false;
    }
    uint64_t mask = type_mask(operand_type);
    uint64_t a = operands[0].imm & mask;
    uint64_t b = operands[1].imm & mask;
    int bits = type_bits(operand_type);
    uint64_t result = 0;

    switch (opcode) {
    case WHIPPET_OP_ADD:
        result = a + b;
        break;
    case WHIPPET_OP_SUB:
        result = a - b;
        break;
    case WHIPPET_OP_MUL:
        result = a * b;
        break;
    case WHIPPET_OP_AND:
        result = a & b;
        break;
    case WHIPPET_OP_OR:
        result = a | b;
        break;
    case WHIPPET_OP_XOR:
        result = a ^ b;
        break;
    case WHIPPET_OP_SHL:
        if (b >= (uint64_t)bits) {
            return false;
        }
        result = a << b;
        break;
    case WHIPPET_OP_LSHR:
        if (b >= (uint64_t)bits) {
            return false;
        }
        result = a >> b;
        break;
    case WHIPPET_OP_ASHR:
        if (b >= (uint64_t)bits) {
            return false;
        }
        result = (uint64_t)(sign_extend_bits(a, operand_type) >> b);
        break;
    case WHIPPET_OP_SDIV:
    case WHIPPET_OP_SREM: {
        int64_t sa = sign_extend_bits(a, operand_type);
        int64_t sb = sign_extend_bits(b, operand_type);
        if (sb == 0) {
            return false;
        }
        if (sa == INT64_MIN && sb == -1) {
            return false;
        }
        result = (uint64_t)(opcode == WHIPPET_OP_SDIV ? sa / sb : sa % sb);
        break;
    }
    case WHIPPET_OP_UDIV:
    case WHIPPET_OP_UREM:
        if (b == 0) {
            return false;
        }
        result = opcode == WHIPPET_OP_UDIV ? a / b : a % b;
        break;
    default:
        return false;
    }
    *out = make_int_operand(result_type, result & type_mask(result_type));
    return true;
}

static bool constant_pass(WhippetModule *module, WhippetFunction *function) {
    bool changed = false;
    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        uint32_t handle = block->first;
        while (handle != WHIPPET_HANDLE_NONE) {
            WhippetInstr *instruction = whippet_module_node(module, handle);
            uint32_t next = instruction->next;
            if (instruction->result != 0 &&
                is_pure_opcode((WhippetOpcode)instruction->opcode) &&
                instruction->operand_count != 0) {
                WhippetOperand *operands =
                    &module->operands[instruction->operands];
                WhippetOperand folded;
                if (fold_instruction(instruction, operands, &folded)) {
                    replace_uses(module, function, instruction->result,
                                 &folded);
                    if (delete_instruction(module, function, handle)) {
                        changed = true;
                    }
                }
            }
            handle = next;
        }
    }
    return changed;
}

static bool strength_pass(WhippetModule *module, WhippetFunction *function) {
    bool changed = false;
    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        uint32_t handle = block->first;
        while (handle != WHIPPET_HANDLE_NONE) {
            WhippetInstr *instruction = whippet_module_node(module, handle);
            if (instruction->opcode == WHIPPET_OP_MUL &&
                instruction->operand_count == 2) {
                WhippetOperand *operands =
                    &module->operands[instruction->operands];
                int constant_index = -1;
                if (operand_is_int(&operands[1]) &&
                    !operand_is_int(&operands[0])) {
                    constant_index = 1;
                } else if (operand_is_int(&operands[0]) &&
                           !operand_is_int(&operands[1])) {
                    constant_index = 0;
                }
                if (constant_index >= 0) {
                    WhippetType type =
                        (WhippetType)instruction->result_type;
                    int bits = type_bits(type);
                    uint64_t value =
                        operands[constant_index].imm & type_mask(type);
                    if (bits != 0 && value > 1 &&
                        (value & (value - 1u)) == 0) {
                        int shift = 0;
                        while (((value >> shift) & 1u) == 0) {
                            shift += 1;
                        }
                        if (shift < bits) {
                            if (constant_index == 0) {
                                WhippetOperand temporary = operands[0];
                                operands[0] = operands[1];
                                operands[1] = temporary;
                            }
                            operands[1] = make_int_operand(type, (uint64_t)shift);
                            instruction->opcode = WHIPPET_OP_SHL;
                            changed = true;
                        }
                    }
                }
            }
            handle = instruction->next;
        }
    }
    return changed;
}

static bool dce_pass(WhippetModule *module, WhippetFunction *function,
                     WhippetArena *scratch) {
    bool any_changed = false;
    bool changed;
    do {
        changed = false;
        uint32_t *uses = compute_uses(module, function, scratch);
        for (WhippetBlock *block = function->first_block; block != NULL;
             block = block->next) {
            uint32_t handle = block->first;
            while (handle != WHIPPET_HANDLE_NONE) {
                WhippetInstr *instruction = whippet_module_node(module, handle);
                uint32_t next = instruction->next;
                if (instruction->result != 0 &&
                    instruction->result <= function->value_count &&
                    uses[instruction->result] == 0 &&
                    is_pure_opcode((WhippetOpcode)instruction->opcode)) {
                    if (delete_instruction(module, function, handle)) {
                        changed = true;
                        any_changed = true;
                    }
                }
                handle = next;
            }
        }
    } while (changed);
    return any_changed;
}

static void optimize_function(WhippetModule *module,
                              WhippetFunction *function,
                              WhippetArena *scratch) {
    for (int round = 0; round < 2; ++round) {
        bool changed = false;
        changed |= constant_pass(module, function);
        changed |= strength_pass(module, function);
        changed |= dce_pass(module, function, scratch);
        if (!changed) {
            break;
        }
    }
}

bool whippet_optimize_module(WhippetModule *module, int level,
                             WhippetDiagnostic *diagnostic) {
    (void)diagnostic;
    if (level <= 0) {
        return true;
    }
    WhippetArena *scratch = whippet_arena_create();
    for (WhippetFunction *function = module->first_function; function != NULL;
         function = function->next) {
        if (!function->is_declaration) {
            optimize_function(module, function, scratch);
        }
    }
    whippet_arena_destroy(scratch);
    return true;
}
