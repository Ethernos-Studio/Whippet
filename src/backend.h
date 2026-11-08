/*
 * Author: dhjs0000
 *
 * Shared machine-level definitions for the M1 x86-64 backend. The backend
 * deliberately targets the Windows x64 ABI first (see design section 7.3);
 * SysV support is a later port.
 */
#ifndef WHIPPET_BACKEND_H
#define WHIPPET_BACKEND_H

#include "whippet/ir.h"

#include "ir/ir_internal.h"

typedef enum MReg {
    MREG_RAX = 0,
    MREG_RCX,
    MREG_RDX,
    MREG_RBX,
    MREG_RBP,
    MREG_RSI,
    MREG_RDI,
    MREG_R8,
    MREG_R9,
    MREG_R10,
    MREG_R11,
    MREG_R12,
    MREG_R13,
    MREG_R14,
    MREG_R15,
    MREG_COUNT
} MReg;

typedef enum MLocationKind {
    MLOC_NONE = 0,
    MLOC_REG,
    MLOC_STACK
} MLocationKind;

typedef struct MLocation {
    uint8_t kind;   /* MLocationKind */
    uint8_t reg;    /* MReg when kind == MLOC_REG */
    int32_t offset; /* rsp-relative byte offset when kind == MLOC_STACK */
} MLocation;

typedef struct MFunction {
    const WhippetModule *module;
    WhippetFunction *function;
    WhippetArena *scratch;

    MLocation *locations; /* indexed by 1-based value id */
    uint32_t frame_size;  /* bytes subtracted from rsp, 16-byte aligned */
    uint32_t param_count;
    uint32_t spill_slots;
    uint32_t scratch_base_slot; /* first phi-edge scratch slot */
    uint32_t max_edge_moves;

    bool saved_callee[MREG_COUNT];
    bool saves_callee_saved;
} MFunction;

/* Linear scan over SSA live intervals; fills locations and frame layout. */
bool whippet_regalloc_run(MFunction *function, WhippetDiagnostic *diagnostic);

/* Naive selection + GNU as (Intel syntax) text emission for a whole module. */
bool whippet_codegen_module_asm(const WhippetModule *module, FILE *out,
                                WhippetDiagnostic *diagnostic);

#endif /* WHIPPET_BACKEND_H */
