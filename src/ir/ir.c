/*
 * Whippet -- independent compiler backend.
 * Author: dhjs0000
 */

#include "whippet/ir.h"

#include "ir_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Type / opcode / predicate tables                                    */
/* ------------------------------------------------------------------ */

static const char *const kTypeNames[WHIPPET_TYPE_COUNT] = {
    "void", "i8", "i16", "i32", "i64", "f32", "f64", "ptr",
};

static const uint32_t kTypeSizes[WHIPPET_TYPE_COUNT] = {
    0, 1, 2, 4, 8, 4, 8, 8,
};

typedef struct WhippetOpcodeInfo {
    const char *name;
    WhippetOpcodeKind kind;
    bool has_result;
} WhippetOpcodeInfo;

static const WhippetOpcodeInfo kOpcodeInfo[WHIPPET_OP_COUNT] = {
    [WHIPPET_OP_RET] = {"ret", WHIPPET_KIND_TERMINATOR, false},
    [WHIPPET_OP_BR] = {"br", WHIPPET_KIND_TERMINATOR, false},
    [WHIPPET_OP_CONDBR] = {"condbr", WHIPPET_KIND_TERMINATOR, false},
    [WHIPPET_OP_SWITCH] = {"switch", WHIPPET_KIND_TERMINATOR, false},

    [WHIPPET_OP_ADD] = {"add", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_SUB] = {"sub", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_MUL] = {"mul", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_SDIV] = {"sdiv", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_UDIV] = {"udiv", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_SREM] = {"srem", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_UREM] = {"urem", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_AND] = {"and", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_OR] = {"or", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_XOR] = {"xor", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_SHL] = {"shl", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_LSHR] = {"lshr", WHIPPET_KIND_BINARY_INT, true},
    [WHIPPET_OP_ASHR] = {"ashr", WHIPPET_KIND_BINARY_INT, true},

    [WHIPPET_OP_NEG] = {"neg", WHIPPET_KIND_UNARY_INT, true},
    [WHIPPET_OP_NOT] = {"not", WHIPPET_KIND_UNARY_INT, true},

    [WHIPPET_OP_FADD] = {"fadd", WHIPPET_KIND_BINARY_FP, true},
    [WHIPPET_OP_FSUB] = {"fsub", WHIPPET_KIND_BINARY_FP, true},
    [WHIPPET_OP_FMUL] = {"fmul", WHIPPET_KIND_BINARY_FP, true},
    [WHIPPET_OP_FDIV] = {"fdiv", WHIPPET_KIND_BINARY_FP, true},
    [WHIPPET_OP_FNEG] = {"fneg", WHIPPET_KIND_UNARY_FP, true},

    [WHIPPET_OP_ICMP] = {"icmp", WHIPPET_KIND_ICMP, true},
    [WHIPPET_OP_FCMP] = {"fcmp", WHIPPET_KIND_FCMP, true},

    [WHIPPET_OP_ALLOCA] = {"alloca", WHIPPET_KIND_ALLOCA, true},
    [WHIPPET_OP_LOAD] = {"load", WHIPPET_KIND_LOAD, true},
    [WHIPPET_OP_STORE] = {"store", WHIPPET_KIND_STORE, false},
    [WHIPPET_OP_PTRADD] = {"ptradd", WHIPPET_KIND_PTRADD, true},

    [WHIPPET_OP_PHI] = {"phi", WHIPPET_KIND_PHI, true},
    [WHIPPET_OP_COPY] = {"copy", WHIPPET_KIND_COPY, true},
    [WHIPPET_OP_SELECT] = {"select", WHIPPET_KIND_SELECT, true},

    [WHIPPET_OP_CALL] = {"call", WHIPPET_KIND_CALL, true},

    [WHIPPET_OP_TRUNC] = {"trunc", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_ZEXT] = {"zext", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_SEXT] = {"sext", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_FPEXT] = {"fpext", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_FPTRUNC] = {"fptrunc", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_BITCAST] = {"bitcast", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_PTRTOINT] = {"ptrtoint", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_INTTOPTR] = {"inttoptr", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_SITOFP] = {"sitofp", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_UITOFP] = {"uitofp", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_FPTOSI] = {"fptosi", WHIPPET_KIND_CAST, true},
    [WHIPPET_OP_FPTOUI] = {"fptoui", WHIPPET_KIND_CAST, true},
};

static const char *const kICmpNames[] = {
    "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge",
};

static const char *const kFCmpNames[] = {
    "oeq", "one", "olt", "ole", "ogt", "oge", "ord",
    "uno", "ueq", "une", "ult", "ule", "ugt", "uge",
};

const char *whippet_type_name(WhippetType type) {
    if (type < 0 || type >= WHIPPET_TYPE_COUNT) {
        return "invalid";
    }
    return kTypeNames[type];
}

bool whippet_type_from_string(const char *text, size_t length, WhippetType *out) {
    for (uint32_t i = 0; i < (uint32_t)WHIPPET_TYPE_COUNT; ++i) {
        size_t name_length = strlen(kTypeNames[i]);
        if (name_length == length && memcmp(text, kTypeNames[i], length) == 0) {
            *out = (WhippetType)i;
            return true;
        }
    }
    return false;
}

bool whippet_type_is_integer(WhippetType type) {
    return type == WHIPPET_TYPE_I8 || type == WHIPPET_TYPE_I16 ||
           type == WHIPPET_TYPE_I32 || type == WHIPPET_TYPE_I64;
}

bool whippet_type_is_float(WhippetType type) {
    return type == WHIPPET_TYPE_F32 || type == WHIPPET_TYPE_F64;
}

bool whippet_type_is_numeric(WhippetType type) {
    return whippet_type_is_integer(type) || whippet_type_is_float(type);
}

uint32_t whippet_type_size(WhippetType type) {
    if (type < 0 || type >= WHIPPET_TYPE_COUNT) {
        return 0;
    }
    return kTypeSizes[type];
}

const char *whippet_opcode_name(WhippetOpcode opcode) {
    if (opcode <= WHIPPET_OP_INVALID || opcode >= WHIPPET_OP_COUNT) {
        return "invalid";
    }
    return kOpcodeInfo[opcode].name;
}

bool whippet_opcode_from_string(const char *text, size_t length,
                                WhippetOpcode *out) {
    for (uint32_t i = 1; i < (uint32_t)WHIPPET_OP_COUNT; ++i) {
        const char *name = kOpcodeInfo[i].name;
        size_t name_length = strlen(name);
        if (name_length == length && memcmp(text, name, length) == 0) {
            *out = (WhippetOpcode)i;
            return true;
        }
    }
    return false;
}

bool whippet_opcode_has_result(WhippetOpcode opcode) {
    if (opcode <= WHIPPET_OP_INVALID || opcode >= WHIPPET_OP_COUNT) {
        return false;
    }
    return kOpcodeInfo[opcode].has_result;
}

WhippetOpcodeKind whippet_opcode_kind(WhippetOpcode opcode) {
    if (opcode <= WHIPPET_OP_INVALID || opcode >= WHIPPET_OP_COUNT) {
        return WHIPPET_KIND_INVALID;
    }
    return kOpcodeInfo[opcode].kind;
}

const char *whippet_predicate_name(WhippetOpcode comparison, uint8_t predicate) {
    if (comparison == WHIPPET_OP_ICMP &&
        predicate < sizeof(kICmpNames) / sizeof(kICmpNames[0])) {
        return kICmpNames[predicate];
    }
    if (comparison == WHIPPET_OP_FCMP &&
        predicate < sizeof(kFCmpNames) / sizeof(kFCmpNames[0])) {
        return kFCmpNames[predicate];
    }
    return NULL;
}

bool whippet_predicate_from_string(WhippetOpcode comparison, const char *text,
                                   size_t length, uint8_t *out) {
    const char *const *names = NULL;
    size_t count = 0;
    if (comparison == WHIPPET_OP_ICMP) {
        names = kICmpNames;
        count = sizeof(kICmpNames) / sizeof(kICmpNames[0]);
    } else if (comparison == WHIPPET_OP_FCMP) {
        names = kFCmpNames;
        count = sizeof(kFCmpNames) / sizeof(kFCmpNames[0]);
    } else {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t name_length = strlen(names[i]);
        if (name_length == length && memcmp(text, names[i], length) == 0) {
            *out = (uint8_t)i;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

#define WHIPPET_ARENA_CHUNK_SIZE (64u * 1024u)

typedef struct WhippetArenaChunk {
    struct WhippetArenaChunk *next;
    size_t used;
    size_t capacity;
    unsigned char *data;
} WhippetArenaChunk;

typedef struct WhippetString {
    char *data;
    uint32_t length;
    uint32_t hash;
    uint32_t next; /* next string handle in the same bucket, 0 terminates */
} WhippetString;

struct WhippetArena {
    WhippetArenaChunk *first_chunk;
    WhippetArenaChunk *current_chunk;

    WhippetString *strings;
    uint32_t string_count;
    uint32_t string_capacity;

    uint32_t *buckets;
    uint32_t bucket_count;
};

static void whippet_out_of_memory(void) {
    fputs("whippet: out of memory\n", stderr);
    exit(1);
}

static WhippetArenaChunk *arena_new_chunk(size_t capacity) {
    WhippetArenaChunk *chunk =
        (WhippetArenaChunk *)malloc(sizeof(WhippetArenaChunk) + capacity);
    if (chunk == NULL) {
        whippet_out_of_memory();
    }
    chunk->next = NULL;
    chunk->used = 0;
    chunk->capacity = capacity;
    chunk->data = (unsigned char *)(chunk + 1);
    return chunk;
}

WhippetArena *whippet_arena_create(void) {
    WhippetArena *arena = (WhippetArena *)calloc(1, sizeof(WhippetArena));
    if (arena == NULL) {
        whippet_out_of_memory();
    }
    arena->first_chunk = arena_new_chunk(WHIPPET_ARENA_CHUNK_SIZE);
    arena->current_chunk = arena->first_chunk;
    return arena;
}

void whippet_arena_destroy(WhippetArena *arena) {
    if (arena == NULL) {
        return;
    }
    WhippetArenaChunk *chunk = arena->first_chunk;
    while (chunk != NULL) {
        WhippetArenaChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    free(arena);
}

void *whippet_arena_alloc(WhippetArena *arena, size_t size, size_t alignment) {
    if (size == 0) {
        size = 1;
    }
    if (alignment < 8) {
        alignment = 8;
    }
    WhippetArenaChunk *chunk = arena->current_chunk;
    size_t offset = (chunk->used + alignment - 1u) & ~(alignment - 1u);
    if (offset + size > chunk->capacity) {
        size_t capacity = size > WHIPPET_ARENA_CHUNK_SIZE ? size
                                                          : WHIPPET_ARENA_CHUNK_SIZE;
        WhippetArenaChunk *fresh = arena_new_chunk(capacity);
        chunk->next = fresh;
        arena->current_chunk = fresh;
        chunk = fresh;
        offset = 0;
    }
    void *result = chunk->data + offset;
    chunk->used = offset + size;
    return result;
}

char *whippet_arena_strndup(WhippetArena *arena, const char *text,
                            size_t length) {
    char *copy = (char *)whippet_arena_alloc(arena, length + 1, 1);
    if (length != 0) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    return copy;
}

static uint32_t whippet_hash_bytes(const char *text, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) {
        hash ^= (uint8_t)text[i];
        hash *= 16777619u;
    }
    return hash;
}

static void arena_rehash(WhippetArena *arena, uint32_t bucket_count) {
    uint32_t *buckets = (uint32_t *)whippet_arena_alloc(
        arena, (size_t)bucket_count * sizeof(uint32_t), 8);
    memset(buckets, 0, (size_t)bucket_count * sizeof(uint32_t));
    for (uint32_t i = 0; i < arena->string_count; ++i) {
        WhippetString *string = &arena->strings[i];
        uint32_t bucket = string->hash & (bucket_count - 1u);
        string->next = buckets[bucket];
        buckets[bucket] = i + 1u;
    }
    arena->buckets = buckets;
    arena->bucket_count = bucket_count;
}

uint32_t whippet_arena_intern(WhippetArena *arena, const char *text,
                              size_t length) {
    if (length == 0) {
        return 0;
    }
    uint32_t hash = whippet_hash_bytes(text, length);

    if (arena->bucket_count == 0) {
        arena_rehash(arena, 256);
    }
    uint32_t bucket = hash & (arena->bucket_count - 1u);
    for (uint32_t handle = arena->buckets[bucket]; handle != 0;
         handle = arena->strings[handle - 1u].next) {
        WhippetString *string = &arena->strings[handle - 1u];
        if (string->hash == hash && string->length == length &&
            memcmp(string->data, text, length) == 0) {
            return handle;
        }
    }

    if (arena->string_count == arena->string_capacity) {
        uint32_t capacity =
            arena->string_capacity == 0 ? 64u : arena->string_capacity * 2u;
        WhippetString *fresh = (WhippetString *)whippet_arena_alloc(
            arena, (size_t)capacity * sizeof(WhippetString), 8);
        if (arena->string_count != 0) {
            memcpy(fresh, arena->strings,
                   (size_t)arena->string_count * sizeof(WhippetString));
        }
        arena->strings = fresh;
        arena->string_capacity = capacity;
    }

    arena->strings[arena->string_count].data =
        whippet_arena_strndup(arena, text, length);
    arena->strings[arena->string_count].length = (uint32_t)length;
    arena->strings[arena->string_count].hash = hash;
    arena->strings[arena->string_count].next = arena->buckets[bucket];
    arena->buckets[bucket] = arena->string_count + 1u;
    arena->string_count += 1u;

    if (arena->string_count * 4u >= arena->bucket_count * 3u) {
        arena_rehash(arena, arena->bucket_count * 2u);
    }
    return arena->string_count;
}

const char *whippet_arena_string(const WhippetArena *arena, uint32_t handle) {
    if (handle == 0 || handle > arena->string_count) {
        return "";
    }
    return arena->strings[handle - 1u].data;
}

uint32_t whippet_arena_string_length(const WhippetArena *arena,
                                     uint32_t handle) {
    if (handle == 0 || handle > arena->string_count) {
        return 0;
    }
    return arena->strings[handle - 1u].length;
}

/* ------------------------------------------------------------------ */
/* Module construction                                                 */
/* ------------------------------------------------------------------ */

bool whippet_vec_push(WhippetArena *arena, void **data, uint32_t *count,
                      uint32_t *capacity, size_t element_size,
                      const void *element) {
    if (*count == *capacity) {
        uint32_t next_capacity = *capacity == 0 ? 8u : *capacity * 2u;
        if (next_capacity <= *capacity) {
            whippet_out_of_memory();
        }
        void *fresh = whippet_arena_alloc(
            arena, (size_t)next_capacity * element_size, 8);
        if (*count != 0) {
            memcpy(fresh, *data, (size_t)*count * element_size);
        }
        *data = fresh;
        *capacity = next_capacity;
    }
    memcpy((unsigned char *)*data + (size_t)*count * element_size, element,
           element_size);
    *count += 1u;
    return true;
}

WhippetModule *whippet_module_create(WhippetArena *arena) {
    WhippetModule *module =
        (WhippetModule *)whippet_arena_alloc(arena, sizeof(WhippetModule), 8);
    memset(module, 0, sizeof(*module));
    module->arena = arena;
    return module;
}

WhippetFunction *whippet_function_create(WhippetModule *module, uint32_t name,
                                         WhippetType return_type,
                                         bool is_declaration) {
    WhippetFunction *function = (WhippetFunction *)whippet_arena_alloc(
        module->arena, sizeof(WhippetFunction), 8);
    memset(function, 0, sizeof(*function));
    function->name = name;
    function->arena = module->arena;
    function->return_type = (uint8_t)return_type;
    function->is_declaration = is_declaration ? 1u : 0u;
    function->id = module->function_count + 1u;

    if (module->last_function != NULL) {
        module->last_function->next = function;
    } else {
        module->first_function = function;
    }
    module->last_function = function;
    module->function_count += 1u;
    return function;
}

uint32_t whippet_node_alloc(WhippetModule *module, WhippetOpcode opcode,
                            uint32_t location) {
    uint32_t handle = module->node_count + 1u;
    uint32_t block_index = module->node_count >> WHIPPET_NODE_BLOCK_SHIFT;
    if (block_index >= module->node_block_capacity) {
        uint32_t next_capacity = module->node_block_capacity == 0
                                     ? 4u
                                     : module->node_block_capacity * 2u;
        WhippetNodeBlock **fresh = (WhippetNodeBlock **)whippet_arena_alloc(
            module->arena, (size_t)next_capacity * sizeof(WhippetNodeBlock *),
            8);
        if (module->node_block_count != 0) {
            memcpy(fresh, module->node_blocks,
                   (size_t)module->node_block_count *
                       sizeof(WhippetNodeBlock *));
        }
        module->node_blocks = fresh;
        module->node_block_capacity = next_capacity;
    }
    if (block_index >= module->node_block_count) {
        WhippetNodeBlock *block = (WhippetNodeBlock *)whippet_arena_alloc(
            module->arena, sizeof(WhippetNodeBlock), 8);
        memset(block, 0, sizeof(*block));
        module->node_blocks[module->node_block_count++] = block;
    }
    WhippetInstr *instruction = whippet_module_node(module, handle);
    memset(instruction, 0, sizeof(*instruction));
    instruction->opcode = (uint16_t)opcode;
    instruction->location = location;
    module->node_count += 1u;
    return handle;
}

uint32_t whippet_operand_push(WhippetModule *module,
                              const WhippetOperand *operand,
                              uint32_t location) {
    if (module->operand_count == module->operand_capacity) {
        uint32_t next_capacity =
            module->operand_capacity == 0 ? 64u : module->operand_capacity * 2u;
        WhippetOperand *fresh = (WhippetOperand *)whippet_arena_alloc(
            module->arena, (size_t)next_capacity * sizeof(WhippetOperand), 8);
        if (module->operand_count != 0) {
            memcpy(fresh, module->operands,
                   (size_t)module->operand_count * sizeof(WhippetOperand));
        }
        module->operands = fresh;

        uint32_t *locations = (uint32_t *)whippet_arena_alloc(
            module->arena, (size_t)next_capacity * sizeof(uint32_t), 8);
        if (module->operand_count != 0) {
            memcpy(locations, module->operand_locations,
                   (size_t)module->operand_count * sizeof(uint32_t));
        }
        module->operand_locations = locations;
        module->operand_capacity = next_capacity;
    }
    uint32_t index = module->operand_count;
    module->operands[index] = *operand;
    module->operand_locations[index] = location;
    module->operand_count += 1u;
    return index;
}

bool whippet_function_add_value(WhippetFunction *function, uint32_t name,
                                WhippetType type, uint32_t def,
                                uint32_t *out_value_id) {
    WhippetValue value;
    memset(&value, 0, sizeof(value));
    value.name = name;
    value.def = def;
    value.type = (uint8_t)type;
    uint32_t id = function->value_count + 1u;
    if (!whippet_vec_push(function->arena, (void **)&function->values,
                          &function->value_count, &function->value_capacity,
                          sizeof(WhippetValue), &value)) {
        return false;
    }
    if (out_value_id != NULL) {
        *out_value_id = id;
    }
    return true;
}

bool whippet_function_add_block(WhippetFunction *function,
                                WhippetBlock *block) {
    block->id = function->block_count + 1u;
    block->next = NULL;
    if (function->last_block != NULL) {
        function->last_block->next = block;
    } else {
        function->first_block = block;
    }
    function->last_block = block;
    if (!whippet_vec_push(function->arena, (void **)&function->block_table,
                          &function->block_count, &function->block_capacity,
                          sizeof(WhippetBlock *), &block)) {
        return false;
    }
    return true;
}

bool whippet_function_add_param(WhippetFunction *function,
                                const WhippetParam *param) {
    return whippet_vec_push(function->arena, (void **)&function->params,
                            &function->param_count, &function->param_capacity,
                            sizeof(WhippetParam), param);
}

/* ------------------------------------------------------------------ */
/* File helpers                                                        */
/* ------------------------------------------------------------------ */

bool whippet_read_file(WhippetArena *arena, const char *path, char **data,
                       size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    char *buffer = (char *)whippet_arena_alloc(arena, (size_t)size + 1u, 1);
    size_t read_size = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        return false;
    }
    buffer[size] = '\0';
    *data = buffer;
    *length = (size_t)size;
    return true;
}

uint32_t whippet_module_function_count(const WhippetModule *module) {
    return module->function_count;
}

/* ------------------------------------------------------------------ */
/* Static layout checks                                                */
/* ------------------------------------------------------------------ */

typedef char whippet_instr_size_check
    [(sizeof(WhippetInstr) == 24) ? 1 : -1];
typedef char whippet_operand_size_check
    [(sizeof(WhippetOperand) == 24) ? 1 : -1];
