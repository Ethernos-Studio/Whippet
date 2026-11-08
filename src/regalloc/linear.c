/*
 * Author: dhjs0000
 *
 * Poletto-Sarkar linear scan for the M1 integer subset.
 *
 * Locations are chosen for every SSA value: a physical register or an
 * rsp-relative stack slot. Phi results are deliberately forced to stack
 * slots; predecessor edges store the incoming value into that slot, which
 * avoids parallel-copy cycles in the naive backend.
 *
 * Register restrictions:
 *   - RAX, RCX, RDX are reserved as instruction-selection scratch registers
 *     (division, shifts, comparison results), so values never live in them.
 *   - The remaining twelve registers are allocatable; RBX/RBP/RSI/RDI/R12-R15
 *     are callee-saved under the Windows x64 ABI and are saved in the
 *     prologue when used.
 */
#include "backend.h"

#include <stdlib.h>
#include <string.h>

typedef struct LiveInterval {
    uint32_t value; /* 1-based SSA value id */
    uint32_t start;
    uint32_t end;
    bool force_stack;
} LiveInterval;

static const MReg kAllocatable[] = {
    MREG_R8,  MREG_R9,  MREG_R10, MREG_R11, MREG_RBX, MREG_RBP,
    MREG_RSI, MREG_RDI, MREG_R12, MREG_R13, MREG_R14, MREG_R15,
};

#define WHIPPET_ALLOCATABLE_COUNT \
    ((uint32_t)(sizeof(kAllocatable) / sizeof(kAllocatable[0])))

static bool reg_is_callee_saved(MReg reg) {
    switch (reg) {
    case MREG_RBX:
    case MREG_RBP:
    case MREG_RSI:
    case MREG_RDI:
    case MREG_R12:
    case MREG_R13:
    case MREG_R14:
    case MREG_R15:
        return true;
    default:
        return false;
    }
}

static int compare_intervals(const void *left, const void *right) {
    const LiveInterval *a = (const LiveInterval *)left;
    const LiveInterval *b = (const LiveInterval *)right;
    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }
    if (a->end != b->end) {
        return a->end < b->end ? -1 : 1;
    }
    if (a->value != b->value) {
        return a->value < b->value ? -1 : 1;
    }
    return 0;
}

static MLocation *value_location(MFunction *function, uint32_t value) {
    return &function->locations[value];
}

static void assign_register(MFunction *function, LiveInterval *interval,
                            MReg reg) {
    MLocation *location = value_location(function, interval->value);
    location->kind = MLOC_REG;
    location->reg = (uint8_t)reg;
    location->offset = 0;
    if (reg_is_callee_saved(reg)) {
        function->saved_callee[reg] = true;
        function->saves_callee_saved = true;
    }
}

static void assign_stack(MFunction *function, LiveInterval *interval,
                         uint32_t *next_slot) {
    uint32_t slot;
    if (interval->value <= function->param_count) {
        /* Parameters have reserved staging slots at the bottom of the frame. */
        slot = interval->value - 1u;
    } else {
        slot = *next_slot;
        *next_slot += 1u;
    }
    MLocation *location = value_location(function, interval->value);
    location->kind = MLOC_STACK;
    location->reg = 0;
    location->offset = (int32_t)(slot * 8u);
}

static void active_insert(uint32_t *active, uint32_t *active_count,
                          uint32_t index, const LiveInterval *intervals) {
    uint32_t position = *active_count;
    while (position > 0 &&
           intervals[active[position - 1u]].end > intervals[index].end) {
        active[position] = active[position - 1u];
        position -= 1u;
    }
    active[position] = index;
    *active_count += 1u;
}

static void bits_set(uint32_t *bits, uint32_t value) {
    uint32_t index = value - 1u;
    bits[index >> 5] |= 1u << (index & 31u);
}

static bool bits_test(const uint32_t *bits, uint32_t value) {
    uint32_t index = value - 1u;
    return (bits[index >> 5] & (1u << (index & 31u))) != 0;
}

static void union_successor(uint32_t *destination, const uint32_t *live_in,
                            uint32_t words, uint32_t block_count,
                            uint32_t target_id) {
    if (target_id == 0 || target_id > block_count) {
        return;
    }
    const uint32_t *source = live_in + (size_t)(target_id - 1u) * words;
    for (uint32_t w = 0; w < words; ++w) {
        destination[w] |= source[w];
    }
}

static void compute_block_successors(const WhippetModule *module,
                                     const WhippetFunction *function,
                                     const WhippetBlock *block,
                                     uint32_t *destination, uint32_t words,
                                     const uint32_t *live_in) {
    if (block->last == WHIPPET_HANDLE_NONE) {
        return;
    }
    const WhippetInstr *terminator = whippet_module_node(module, block->last);
    const WhippetOperand *operands =
        whippet_instr_operands(module, terminator);
    WhippetOpcode opcode = (WhippetOpcode)terminator->opcode;
    if (opcode == WHIPPET_OP_BR) {
        union_successor(destination, live_in, words, function->block_count,
                        operands[0].resolved);
    } else if (opcode == WHIPPET_OP_CONDBR) {
        union_successor(destination, live_in, words, function->block_count,
                        operands[1].resolved);
        union_successor(destination, live_in, words, function->block_count,
                        operands[2].resolved);
    } else if (opcode == WHIPPET_OP_SWITCH) {
        for (uint16_t i = 1; i < terminator->operand_count; ++i) {
            union_successor(destination, live_in, words,
                            function->block_count, operands[i].resolved);
        }
    }
}

/*
 * Backward liveness over the CFG. A naive linear interval derived only from
 * layout order is wrong for loops: a value used near the loop head is live
 * across the back edge even though its last textual use is earlier. We extend
 * every value that is live-in or live-out of a block through that block.
 */
static void extend_intervals_with_liveness(
    MFunction *function, const uint32_t *block_start,
    const uint32_t *block_end, LiveInterval *intervals, uint32_t *interval_of) {
    WhippetFunction *fn = function->function;
    const WhippetModule *module = function->module;
    uint32_t value_count = fn->value_count;
    uint32_t block_count = fn->block_count;
    if (value_count == 0 || block_count == 0) {
        return;
    }

    uint32_t words = (value_count + 31u) / 32u;
    size_t bitmap_size = (size_t)block_count * words * sizeof(uint32_t);
    uint32_t *uses = (uint32_t *)whippet_arena_alloc(
        function->scratch, bitmap_size, 8);
    uint32_t *defs = (uint32_t *)whippet_arena_alloc(
        function->scratch, bitmap_size, 8);
    uint32_t *live_in = (uint32_t *)whippet_arena_alloc(
        function->scratch, bitmap_size, 8);
    uint32_t *live_out = (uint32_t *)whippet_arena_alloc(
        function->scratch, bitmap_size, 8);
    uint32_t *temporary = (uint32_t *)whippet_arena_alloc(
        function->scratch, (size_t)words * sizeof(uint32_t), 8);
    memset(uses, 0, bitmap_size);
    memset(defs, 0, bitmap_size);
    memset(live_in, 0, bitmap_size);
    memset(live_out, 0, bitmap_size);

    for (uint32_t b = 0; b < block_count; ++b) {
        WhippetBlock *block = fn->block_table[b];
        uint32_t *block_defs = defs + (size_t)b * words;
        const WhippetInstr *instruction = NULL;
        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(module, handle);
            if (instruction->result != 0) {
                bits_set(block_defs, instruction->result);
            }
        }
    }

    for (uint32_t b = 0; b < block_count; ++b) {
        WhippetBlock *block = fn->block_table[b];
        uint32_t *block_uses = uses + (size_t)b * words;
        const uint32_t *block_defs = defs + (size_t)b * words;
        const WhippetInstr *instruction = NULL;
        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(module, handle);
            if ((WhippetOpcode)instruction->opcode == WHIPPET_OP_PHI) {
                /* Phi operands are edge uses, not uses inside this block. */
                continue;
            }
            const WhippetOperand *operands =
                whippet_instr_operands(module, instruction);
            for (uint16_t i = 0; i < instruction->operand_count; ++i) {
                if (operands[i].kind == WHIPPET_OPERAND_VALUE &&
                    operands[i].resolved != 0 &&
                    !bits_test(block_defs, operands[i].resolved)) {
                    bits_set(block_uses, operands[i].resolved);
                }
            }
        }
    }

    bool changed = true;
    uint32_t iterations = 0;
    while (changed && iterations <= block_count + 1u) {
        changed = false;
        iterations += 1u;
        for (uint32_t b = 0; b < block_count; ++b) {
            WhippetBlock *block = fn->block_table[b];
            memset(temporary, 0, (size_t)words * sizeof(uint32_t));
            compute_block_successors(module, fn, block, temporary, words,
                                     live_in);

            uint32_t *block_in = live_in + (size_t)b * words;
            uint32_t *block_out = live_out + (size_t)b * words;
            const uint32_t *block_uses = uses + (size_t)b * words;
            const uint32_t *block_defs = defs + (size_t)b * words;

            if (memcmp(block_out, temporary,
                       (size_t)words * sizeof(uint32_t)) != 0) {
                memcpy(block_out, temporary,
                       (size_t)words * sizeof(uint32_t));
                changed = true;
            }
            for (uint32_t w = 0; w < words; ++w) {
                uint32_t next_in =
                    block_uses[w] | (temporary[w] & ~block_defs[w]);
                if (block_in[w] != next_in) {
                    block_in[w] = next_in;
                    changed = true;
                }
            }
        }
    }

    for (uint32_t b = 0; b < block_count; ++b) {
        const uint32_t *block_in = live_in + (size_t)b * words;
        const uint32_t *block_out = live_out + (size_t)b * words;
        for (uint32_t w = 0; w < words; ++w) {
            uint32_t pending = block_in[w] | block_out[w];
            while (pending != 0) {
                uint32_t bit = 0;
                uint32_t scan = pending;
                while ((scan & 1u) == 0) {
                    scan >>= 1u;
                    bit += 1u;
                }
                uint32_t value = w * 32u + bit + 1u;
                pending &= pending - 1u;
                if (value > value_count) {
                    continue;
                }
                uint32_t index = interval_of[value];
                if (block_end[b] > intervals[index].end) {
                    intervals[index].end = block_end[b];
                }
                if (block_start[b] < intervals[index].start) {
                    intervals[index].start = block_start[b];
                }
            }
        }
    }
}

static void collect_interval_uses(MFunction *function,
                                  const uint32_t *block_start,
                                  const uint32_t *block_end,
                                  LiveInterval *intervals,
                                  uint32_t *interval_of) {
    const WhippetModule *module = function->module;
    WhippetFunction *fn = function->function;

    for (uint32_t b = 0; b < fn->block_count; ++b) {
        WhippetBlock *block = fn->block_table[b];
        uint32_t position = block_start[b];
        const WhippetInstr *instruction = NULL;

        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(module, handle);
            const WhippetOperand *operands =
                whippet_instr_operands(module, instruction);
            WhippetOpcode opcode = (WhippetOpcode)instruction->opcode;

            if (instruction->result != 0) {
                uint32_t index = interval_of[instruction->result];
                uint32_t start =
                    opcode == WHIPPET_OP_PHI ? block_start[b] : position;
                if (start < intervals[index].start) {
                    intervals[index].start = start;
                }
                if (intervals[index].end < start) {
                    intervals[index].end = start;
                }
                intervals[index].force_stack = opcode == WHIPPET_OP_PHI;
            }

            if (opcode == WHIPPET_OP_PHI) {
                for (uint16_t i = 0; i + 1u < instruction->operand_count;
                     i += 2u) {
                    if (operands[i].kind == WHIPPET_OPERAND_VALUE) {
                        uint32_t pred_id = operands[i + 1u].resolved;
                        if (pred_id >= 1u && pred_id <= fn->block_count &&
                            operands[i].resolved != 0) {
                            uint32_t use_position = block_end[pred_id - 1u];
                            uint32_t index =
                                interval_of[operands[i].resolved];
                            if (use_position > intervals[index].end) {
                                intervals[index].end = use_position;
                            }
                        }
                    }
                }
            } else {
                for (uint16_t i = 0; i < instruction->operand_count; ++i) {
                    if (operands[i].kind == WHIPPET_OPERAND_VALUE &&
                        operands[i].resolved != 0) {
                        uint32_t index = interval_of[operands[i].resolved];
                        if (position > intervals[index].end) {
                            intervals[index].end = position;
                        }
                    }
                }
            }
            position += 1u;
        }
    }
}

bool whippet_regalloc_run(MFunction *function, WhippetDiagnostic *diagnostic) {
    WhippetFunction *fn = function->function;
    WhippetArena *scratch = function->scratch;
    uint32_t value_count = fn->value_count;
    uint32_t block_count = fn->block_count;

    uint32_t *block_start = (uint32_t *)whippet_arena_alloc(
        scratch, (size_t)(block_count == 0 ? 1 : block_count) * sizeof(uint32_t),
        8);
    uint32_t *block_end = (uint32_t *)whippet_arena_alloc(
        scratch, (size_t)(block_count == 0 ? 1 : block_count) * sizeof(uint32_t),
        8);

    uint32_t position = 0;
    for (uint32_t b = 0; b < block_count; ++b) {
        WhippetBlock *block = fn->block_table[b];
        block_start[b] = position;
        const WhippetInstr *instruction = NULL;
        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(function->module, handle);
            position += 1u;
        }
        block_end[b] = position;
    }

    LiveInterval *intervals = (LiveInterval *)whippet_arena_alloc(
        scratch, (size_t)(value_count == 0 ? 1 : value_count) *
                     sizeof(LiveInterval),
        8);
    uint32_t *interval_of = (uint32_t *)whippet_arena_alloc(
        scratch, (size_t)(value_count + 1u) * sizeof(uint32_t), 8);
    memset(interval_of, 0, (size_t)(value_count + 1u) * sizeof(uint32_t));

    for (uint32_t i = 0; i < value_count; ++i) {
        intervals[i].value = i + 1u;
        intervals[i].start = UINT32_MAX;
        intervals[i].end = 0;
        intervals[i].force_stack = false;
        interval_of[i + 1u] = i;
    }

    for (uint32_t i = 0; i < fn->param_count; ++i) {
        uint32_t value = fn->params[i].value;
        if (value != 0) {
            intervals[interval_of[value]].start = 0;
            intervals[interval_of[value]].end = 0;
        }
    }

    collect_interval_uses(function, block_start, block_end, intervals,
                          interval_of);
    extend_intervals_with_liveness(function, block_start, block_end, intervals,
                                   interval_of);

    for (uint32_t i = 0; i < value_count; ++i) {
        if (intervals[i].start == UINT32_MAX) {
            intervals[i].start = 0;
        }
        if (intervals[i].end < intervals[i].start) {
            intervals[i].end = intervals[i].start;
        }
    }

    function->param_count = fn->param_count;
    function->locations = (MLocation *)whippet_arena_alloc(
        scratch, (size_t)(value_count + 1u) * sizeof(MLocation), 8);
    memset(function->locations, 0,
           (size_t)(value_count + 1u) * sizeof(MLocation));

    if (value_count != 0) {
        qsort(intervals, value_count, sizeof(LiveInterval),
              compare_intervals);
    }

    MReg free_regs[WHIPPET_ALLOCATABLE_COUNT];
    uint32_t free_count = 0;
    /* Push in reverse so the first pop gives a caller-saved register. */
    for (uint32_t i = WHIPPET_ALLOCATABLE_COUNT; i > 0; --i) {
        free_regs[free_count++] = kAllocatable[i - 1u];
    }

    uint32_t *active = (uint32_t *)whippet_arena_alloc(
        scratch, (size_t)(value_count == 0 ? 1 : value_count) * sizeof(uint32_t),
        8);
    uint32_t active_count = 0;

    /* Reserved parameter staging slots occupy [0, param_count). */
    uint32_t next_slot = fn->param_count;

    for (uint32_t i = 0; i < value_count; ++i) {
        LiveInterval *interval = &intervals[i];

        if (interval->force_stack) {
            assign_stack(function, interval, &next_slot);
            continue;
        }

        while (active_count > 0 &&
               intervals[active[0]].end < interval->start) {
            MLocation *expired =
                value_location(function, intervals[active[0]].value);
            if (expired->kind == MLOC_REG) {
                free_regs[free_count++] = (MReg)expired->reg;
            }
            memmove(&active[0], &active[1],
                    (size_t)(active_count - 1u) * sizeof(uint32_t));
            active_count -= 1u;
        }

        if (free_count > 0) {
            MReg reg = free_regs[--free_count];
            assign_register(function, interval, reg);
            active_insert(active, &active_count, i, intervals);
            continue;
        }

        if (active_count > 0) {
            uint32_t victim_index = active[active_count - 1u];
            LiveInterval *victim = &intervals[victim_index];
            if (victim->end > interval->end) {
                MLocation *victim_location =
                    value_location(function, victim->value);
                MReg reg = (MReg)victim_location->reg;
                assign_stack(function, victim, &next_slot);
                active_count -= 1u;
                assign_register(function, interval, reg);
                active_insert(active, &active_count, i, intervals);
                continue;
            }
        }

        assign_stack(function, interval, &next_slot);
    }

    function->spill_slots = next_slot - function->param_count;
    function->scratch_base_slot = next_slot;
    uint32_t total_slots = next_slot + function->max_edge_moves;
    uint32_t frame_bytes = total_slots * 8u;
    function->frame_size = (frame_bytes + 15u) & ~15u;

    (void)diagnostic;
    return true;
}
