/*
 * Author: dhjs0000
 *
 * M0 parser for Whippet IR text.
 *
 * The parser is deliberately strict: it is the only gate between a frontend
 * and the backend. It rejects malformed syntax, unknown opcodes/types,
 * duplicate definitions, undefined values or blocks, misplaced phis,
 * unterminated blocks and type mismatches -- with a line/column diagnostic.
 *
 * SSA dominance is a frontend responsibility (design section 3.2); this
 * parser only checks that references exist and have the expected type.
 */
#include "whippet/ir.h"

#include "ir_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */

typedef enum TokenKind {
    TOKEN_EOF = 0,
    TOKEN_INVALID,
    TOKEN_IDENT,
    TOKEN_LOCAL,  /* %name */
    TOKEN_SYMBOL, /* @name */
    TOKEN_INT,
    TOKEN_FLOAT,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_EQUAL,
    TOKEN_ARROW
} TokenKind;

typedef struct Token {
    TokenKind kind;
    const char *start;
    size_t length;
    uint32_t line;
    uint32_t column;
    uint64_t int_value;
    double float_value;
} Token;

typedef struct Lexer {
    const char *source;
    size_t length;
    size_t position;
    uint32_t line;
    uint32_t column;
} Lexer;

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return (uint32_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint32_t)(c - 'a') + 10u;
    }
    return (uint32_t)(c - 'A') + 10u;
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '.' || c == '$';
}

static bool is_ident_continue(char c) {
    return is_ident_start(c) || is_digit(c);
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

static char lexer_peek(const Lexer *lexer, size_t offset) {
    size_t index = lexer->position + offset;
    if (index >= lexer->length) {
        return '\0';
    }
    return lexer->source[index];
}

static void lexer_advance(Lexer *lexer) {
    if (lexer->position >= lexer->length) {
        return;
    }
    char c = lexer->source[lexer->position++];
    if (c == '\n') {
        lexer->line += 1u;
        lexer->column = 1u;
    } else {
        lexer->column += 1u;
    }
}

static void token_begin(Token *token, const Lexer *lexer, TokenKind kind,
                        size_t start, uint32_t line, uint32_t column) {
    token->kind = kind;
    token->start = lexer->source + start;
    token->length = lexer->position - start;
    token->line = line;
    token->column = column;
    token->int_value = 0;
    token->float_value = 0.0;
}

static bool lexer_error(Token *token, const Lexer *lexer, char *error,
                        size_t error_size, const char *message) {
    token->kind = TOKEN_INVALID;
    token->start = lexer->source + lexer->position;
    token->length = 0;
    token->line = lexer->line;
    token->column = lexer->column;
    snprintf(error, error_size, "%s", message);
    return false;
}

static bool lex_number(Lexer *lexer, Token *token, char *error,
                       size_t error_size) {
    size_t start = lexer->position;
    uint32_t line = lexer->line;
    uint32_t column = lexer->column;
    bool negative = false;

    if (lexer_peek(lexer, 0) == '-') {
        negative = true;
        lexer_advance(lexer);
    }

    if (lexer_peek(lexer, 0) == '0' &&
        (lexer_peek(lexer, 1) == 'x' || lexer_peek(lexer, 1) == 'X')) {
        lexer_advance(lexer);
        lexer_advance(lexer);
        if (!is_hex_digit(lexer_peek(lexer, 0))) {
            return lexer_error(token, lexer, error, error_size,
                               "hexadecimal literal has no digits");
        }
        uint64_t value = 0;
        while (is_hex_digit(lexer_peek(lexer, 0))) {
            uint32_t digit = hex_value(lexer_peek(lexer, 0));
            if (value > (UINT64_MAX - digit) / 16u) {
                return lexer_error(token, lexer, error, error_size,
                                   "integer literal is out of range");
            }
            value = value * 16u + digit;
            lexer_advance(lexer);
        }
        uint64_t limit = negative ? (UINT64_C(1) << 63) : UINT64_C(9223372036854775807);
        if (value > limit) {
            return lexer_error(token, lexer, error, error_size,
                               "integer literal is out of range");
        }
        token->kind = TOKEN_INT;
        token->start = lexer->source + start;
        token->length = lexer->position - start;
        token->line = line;
        token->column = column;
        token->int_value = negative ? (uint64_t)(0u - value) : value;
        token->float_value = 0.0;
        return true;
    }

    if (!is_digit(lexer_peek(lexer, 0))) {
        return lexer_error(token, lexer, error, error_size,
                           "'-' must be followed by a number");
    }
    while (is_digit(lexer_peek(lexer, 0))) {
        lexer_advance(lexer);
    }

    bool is_float = false;
    if (lexer_peek(lexer, 0) == '.') {
        is_float = true;
        lexer_advance(lexer);
        while (is_digit(lexer_peek(lexer, 0))) {
            lexer_advance(lexer);
        }
    }
    if (lexer_peek(lexer, 0) == 'e' || lexer_peek(lexer, 0) == 'E') {
        is_float = true;
        lexer_advance(lexer);
        if (lexer_peek(lexer, 0) == '+' || lexer_peek(lexer, 0) == '-') {
            lexer_advance(lexer);
        }
        if (!is_digit(lexer_peek(lexer, 0))) {
            return lexer_error(token, lexer, error, error_size,
                               "exponent has no digits");
        }
        while (is_digit(lexer_peek(lexer, 0))) {
            lexer_advance(lexer);
        }
    }

    size_t length = lexer->position - start;
    if (is_float) {
        if (length >= 128u) {
            return lexer_error(token, lexer, error, error_size,
                               "floating-point literal is too long");
        }
        char buffer[128];
        memcpy(buffer, lexer->source + start, length);
        buffer[length] = '\0';
        char *end = NULL;
        double value = strtod(buffer, &end);
        if (end != buffer + length) {
            return lexer_error(token, lexer, error, error_size,
                               "malformed floating-point literal");
        }
        if (!isfinite(value)) {
            return lexer_error(token, lexer, error, error_size,
                               "floating-point literal is out of range");
        }
        token->kind = TOKEN_FLOAT;
        token->float_value = value;
    } else {
        uint64_t value = 0;
        for (size_t i = (negative ? 1u : 0u); i < length; ++i) {
            uint32_t digit = (uint32_t)(lexer->source[start + i] - '0');
            if (value > (UINT64_MAX - digit) / 10u) {
                return lexer_error(token, lexer, error, error_size,
                                   "integer literal is out of range");
            }
            value = value * 10u + digit;
        }
        uint64_t limit = negative ? (UINT64_C(1) << 63) : UINT64_C(9223372036854775807);
        if (value > limit) {
            return lexer_error(token, lexer, error, error_size,
                               "integer literal is out of range");
        }
        token->kind = TOKEN_INT;
        token->int_value = negative ? (uint64_t)(0u - value) : value;
    }

    token->start = lexer->source + start;
    token->length = length;
    token->line = line;
    token->column = column;
    return true;
}

static void lex_skip_trivia(Lexer *lexer) {
    for (;;) {
        while (lexer->position < lexer->length &&
               is_space(lexer->source[lexer->position])) {
            lexer_advance(lexer);
        }
        if (lexer_peek(lexer, 0) == ';') {
            while (lexer->position < lexer->length &&
                   lexer->source[lexer->position] != '\n') {
                lexer_advance(lexer);
            }
            continue;
        }
        if (lexer_peek(lexer, 0) == '/' && lexer_peek(lexer, 1) == '/') {
            while (lexer->position < lexer->length &&
                   lexer->source[lexer->position] != '\n') {
                lexer_advance(lexer);
            }
            continue;
        }
        break;
    }
}

static bool lexer_next(Lexer *lexer, Token *token, char *error,
                       size_t error_size) {
    lex_skip_trivia(lexer);

    uint32_t line = lexer->line;
    uint32_t column = lexer->column;
    size_t start = lexer->position;

    if (lexer->position >= lexer->length) {
        token->kind = TOKEN_EOF;
        token->start = lexer->source + lexer->length;
        token->length = 0;
        token->line = line;
        token->column = column;
        token->int_value = 0;
        token->float_value = 0.0;
        return true;
    }

    char c = lexer_peek(lexer, 0);

    if (c == '%' || c == '@') {
        bool local = c == '%';
        lexer_advance(lexer);
        size_t name_start = lexer->position;
        if (!is_ident_start(lexer_peek(lexer, 0)) &&
            !(local && is_digit(lexer_peek(lexer, 0)))) {
            return lexer_error(token, lexer, error, error_size,
                               local ? "expected a value name after '%'"
                                     : "expected a symbol name after '@'");
        }
        while (is_ident_continue(lexer_peek(lexer, 0))) {
            lexer_advance(lexer);
        }
        token->kind = local ? TOKEN_LOCAL : TOKEN_SYMBOL;
        token->start = lexer->source + name_start;
        token->length = lexer->position - name_start;
        token->line = line;
        token->column = column;
        token->int_value = 0;
        token->float_value = 0.0;
        return true;
    }

    if (is_ident_start(c)) {
        lexer_advance(lexer);
        while (is_ident_continue(lexer_peek(lexer, 0))) {
            lexer_advance(lexer);
        }
        token_begin(token, lexer, TOKEN_IDENT, start, line, column);
        return true;
    }

    if (is_digit(c) || (c == '-' && is_digit(lexer_peek(lexer, 1)))) {
        return lex_number(lexer, token, error, error_size);
    }

    switch (c) {
    case '{':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_LBRACE, start, line, column);
        return true;
    case '}':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_RBRACE, start, line, column);
        return true;
    case '(':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_LPAREN, start, line, column);
        return true;
    case ')':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_RPAREN, start, line, column);
        return true;
    case '[':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_LBRACKET, start, line, column);
        return true;
    case ']':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_RBRACKET, start, line, column);
        return true;
    case ',':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_COMMA, start, line, column);
        return true;
    case ':':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_COLON, start, line, column);
        return true;
    case '=':
        lexer_advance(lexer);
        token_begin(token, lexer, TOKEN_EQUAL, start, line, column);
        return true;
    case '-':
        if (lexer_peek(lexer, 1) == '>') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            token_begin(token, lexer, TOKEN_ARROW, start, line, column);
            return true;
        }
        return lexer_error(token, lexer, error, error_size,
                           "unexpected character '-'");
    default:
        break;
    }

    char message[64];
    if (c >= 32 && c < 127) {
        snprintf(message, sizeof(message), "unexpected character '%c'", c);
    } else {
        snprintf(message, sizeof(message), "unexpected byte 0x%02X",
                 (unsigned)(unsigned char)c);
    }
    return lexer_error(token, lexer, error, error_size, message);
}

/* ------------------------------------------------------------------ */
/* Name map: interned-name handle -> payload                           */
/* ------------------------------------------------------------------ */

typedef struct NameEntry {
    uint32_t name;
    uint32_t payload;
    uint32_t next; /* 1-based entry index, 0 terminates */
} NameEntry;

typedef struct NameMap {
    NameEntry *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;
    uint32_t *buckets;
    uint32_t bucket_count;
} NameMap;

static uint32_t mix_handle(uint32_t handle) {
    handle ^= handle >> 16;
    handle *= 0x7feb352du;
    handle ^= handle >> 15;
    return handle;
}

static void name_map_init(NameMap *map, WhippetArena *arena) {
    memset(map, 0, sizeof(*map));
    map->bucket_count = 256;
    map->buckets = (uint32_t *)whippet_arena_alloc(
        arena, (size_t)map->bucket_count * sizeof(uint32_t), 8);
    memset(map->buckets, 0, (size_t)map->bucket_count * sizeof(uint32_t));
}

static void name_map_rehash(NameMap *map, WhippetArena *arena,
                            uint32_t bucket_count) {
    uint32_t *buckets = (uint32_t *)whippet_arena_alloc(
        arena, (size_t)bucket_count * sizeof(uint32_t), 8);
    memset(buckets, 0, (size_t)bucket_count * sizeof(uint32_t));
    for (uint32_t i = 0; i < map->entry_count; ++i) {
        uint32_t bucket = mix_handle(map->entries[i].name) & (bucket_count - 1u);
        map->entries[i].next = buckets[bucket];
        buckets[bucket] = i + 1u;
    }
    map->buckets = buckets;
    map->bucket_count = bucket_count;
}

static bool name_map_lookup(const NameMap *map, uint32_t name,
                            uint32_t *payload) {
    if (map->bucket_count == 0) {
        return false;
    }
    uint32_t bucket = mix_handle(name) & (map->bucket_count - 1u);
    for (uint32_t index = map->buckets[bucket]; index != 0;
         index = map->entries[index - 1u].next) {
        if (map->entries[index - 1u].name == name) {
            if (payload != NULL) {
                *payload = map->entries[index - 1u].payload;
            }
            return true;
        }
    }
    return false;
}

static void name_map_insert(NameMap *map, WhippetArena *arena, uint32_t name,
                            uint32_t payload) {
    if (map->entry_count == map->entry_capacity) {
        uint32_t capacity =
            map->entry_capacity == 0 ? 64u : map->entry_capacity * 2u;
        NameEntry *entries = (NameEntry *)whippet_arena_alloc(
            arena, (size_t)capacity * sizeof(NameEntry), 8);
        if (map->entry_count != 0) {
            memcpy(entries, map->entries,
                   (size_t)map->entry_count * sizeof(NameEntry));
        }
        map->entries = entries;
        map->entry_capacity = capacity;
    }
    if ((map->entry_count + 1u) * 4u >= map->bucket_count * 3u) {
        name_map_rehash(map, arena, map->bucket_count * 2u);
    }
    uint32_t bucket = mix_handle(name) & (map->bucket_count - 1u);
    NameEntry *entry = &map->entries[map->entry_count];
    entry->name = name;
    entry->payload = payload;
    entry->next = map->buckets[bucket];
    map->buckets[bucket] = map->entry_count + 1u;
    map->entry_count += 1u;
}

static void name_map_reset(NameMap *map) {
    if (map->bucket_count != 0) {
        memset(map->buckets, 0,
               (size_t)map->bucket_count * sizeof(uint32_t));
    }
    map->entry_count = 0;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct Parser {
    WhippetArena *arena;
    WhippetModule *module;
    Lexer lexer;
    Token token;
    WhippetDiagnostic *diagnostic;
    bool failed;

    NameMap functions;
    NameMap values;
    NameMap blocks;

    WhippetFunction *function;
    WhippetBlock *block;
    bool block_terminated;
    bool block_seen_non_phi;
    bool has_block;
    uint32_t last_operand_location;
} Parser;

static void set_diagnostic(Parser *parser, uint32_t line, uint32_t column,
                           const char *message) {
    if (parser->diagnostic == NULL) {
        return;
    }
    parser->diagnostic->line = line;
    parser->diagnostic->column = column;
    snprintf(parser->diagnostic->message, sizeof(parser->diagnostic->message),
             "%s", message);
}

static void describe_token(const Token *token, char *buffer, size_t size) {
    switch (token->kind) {
    case TOKEN_EOF:
        snprintf(buffer, size, "end of file");
        break;
    case TOKEN_IDENT:
        snprintf(buffer, size, "'%.*s'", (int)token->length, token->start);
        break;
    case TOKEN_LOCAL:
        snprintf(buffer, size, "'%%%.*s'", (int)token->length, token->start);
        break;
    case TOKEN_SYMBOL:
        snprintf(buffer, size, "'@%.*s'", (int)token->length, token->start);
        break;
    case TOKEN_INT:
    case TOKEN_FLOAT:
        snprintf(buffer, size, "'%.*s'", (int)token->length, token->start);
        break;
    case TOKEN_INVALID:
        snprintf(buffer, size, "invalid token");
        break;
    default:
        snprintf(buffer, size, "'%.*s'", (int)token->length, token->start);
        break;
    }
}

static bool parser_error(Parser *parser, const Token *token,
                         const char *format, ...) {
    if (!parser->failed) {
        parser->failed = true;
        char message[sizeof(parser->diagnostic->message)];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);
        set_diagnostic(parser, token->line, token->column, message);
    }
    return false;
}

static bool parser_error_loc(Parser *parser, uint32_t location,
                             const char *format, ...) {
    if (!parser->failed) {
        parser->failed = true;
        char message[sizeof(parser->diagnostic->message)];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);
        set_diagnostic(parser, WHIPPET_LOC_LINE(location),
                       WHIPPET_LOC_COLUMN(location), message);
    }
    return false;
}

static bool parser_advance(Parser *parser) {
    Token token;
    char error[128];
    if (!lexer_next(&parser->lexer, &token, error, sizeof(error))) {
        parser->failed = true;
        set_diagnostic(parser, token.line, token.column, error);
        return false;
    }
    parser->token = token;
    return true;
}

static bool token_is_word(const Token *token, const char *word) {
    if (token->kind != TOKEN_IDENT) {
        return false;
    }
    size_t length = strlen(word);
    return token->length == length &&
           memcmp(token->start, word, length) == 0;
}

static bool expect(Parser *parser, TokenKind kind, const char *what) {
    if (parser->token.kind != kind) {
        char shown[64];
        describe_token(&parser->token, shown, sizeof(shown));
        return parser_error(parser, &parser->token, "expected %s, found %s",
                            what, shown);
    }
    return parser_advance(parser);
}

static bool expect_word(Parser *parser, const char *word) {
    if (!token_is_word(&parser->token, word)) {
        char shown[64];
        describe_token(&parser->token, shown, sizeof(shown));
        return parser_error(parser, &parser->token, "expected '%s', found %s",
                            word, shown);
    }
    return parser_advance(parser);
}

static bool parse_type(Parser *parser, WhippetType *out, bool allow_void) {
    if (parser->token.kind != TOKEN_IDENT) {
        char shown[64];
        describe_token(&parser->token, shown, sizeof(shown));
        return parser_error(parser, &parser->token,
                            "expected a type name, found %s", shown);
    }
    WhippetType type;
    if (!whippet_type_from_string(parser->token.start, parser->token.length,
                                  &type)) {
        return parser_error(parser, &parser->token, "unknown type '%.*s'",
                            (int)parser->token.length, parser->token.start);
    }
    if (!allow_void && type == WHIPPET_TYPE_VOID) {
        return parser_error(parser, &parser->token,
                            "'void' is not a valid operand type");
    }
    *out = type;
    return parser_advance(parser);
}

static bool parse_typed_operand(Parser *parser, WhippetType type,
                                WhippetOperand *out) {
    parser->last_operand_location =
        WHIPPET_LOC_PACK(parser->token.line, parser->token.column);
    memset(out, 0, sizeof(*out));
    out->type = (uint8_t)type;
    out->resolved = 0;
    out->imm = 0;

    switch (parser->token.kind) {
    case TOKEN_LOCAL:
        out->kind = WHIPPET_OPERAND_VALUE;
        out->name = whippet_arena_intern(parser->arena, parser->token.start,
                                         parser->token.length);
        return parser_advance(parser);

    case TOKEN_INT:
        if (type == WHIPPET_TYPE_PTR || whippet_type_is_float(type)) {
            return parser_error(parser, &parser->token,
                                "expected a %s operand, found integer literal",
                                whippet_type_name(type));
        }
        out->kind = WHIPPET_OPERAND_INT;
        out->imm = parser->token.int_value;
        return parser_advance(parser);

    case TOKEN_FLOAT:
        if (!whippet_type_is_float(type)) {
            return parser_error(
                parser, &parser->token,
                "expected a %s operand, found floating-point literal",
                whippet_type_name(type));
        }
        out->kind = WHIPPET_OPERAND_FLOAT;
        memcpy(&out->imm, &parser->token.float_value, sizeof(double));
        return parser_advance(parser);

    default: {
        char shown[64];
        describe_token(&parser->token, shown, sizeof(shown));
        return parser_error(parser, &parser->token,
                            "expected a %s operand, found %s",
                            whippet_type_name(type), shown);
    }
    }
}

static bool parse_block_operand(Parser *parser, WhippetOperand *out) {
    parser->last_operand_location =
        WHIPPET_LOC_PACK(parser->token.line, parser->token.column);
    memset(out, 0, sizeof(*out));
    if (parser->token.kind != TOKEN_IDENT) {
        char shown[64];
        describe_token(&parser->token, shown, sizeof(shown));
        return parser_error(parser, &parser->token,
                            "expected a block label, found %s", shown);
    }
    out->kind = WHIPPET_OPERAND_BLOCK;
    out->type = WHIPPET_TYPE_VOID;
    out->name = whippet_arena_intern(parser->arena, parser->token.start,
                                     parser->token.length);
    return parser_advance(parser);
}

static bool push_operand(Parser *parser, const WhippetOperand *operand) {
    whippet_operand_push(parser->module, operand,
                         parser->last_operand_location);
    return true;
}

static bool peek_is_colon(Parser *parser) {
    if (parser->token.kind != TOKEN_IDENT) {
        return false;
    }
    Lexer saved = parser->lexer;
    Token next;
    char error[64];
    bool result = lexer_next(&parser->lexer, &next, error, sizeof(error)) &&
                  next.kind == TOKEN_COLON;
    parser->lexer = saved;
    return result;
}

/* ------------------------------------------------------------------ */
/* Function parsing                                                    */
/* ------------------------------------------------------------------ */

static bool parse_params(Parser *parser, WhippetFunction *function) {
    if (!expect(parser, TOKEN_LPAREN, "'('")) {
        return false;
    }
    if (parser->token.kind == TOKEN_RPAREN) {
        return parser_advance(parser);
    }
    for (;;) {
        WhippetType type;
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        if (parser->token.kind != TOKEN_LOCAL) {
            char shown[64];
            describe_token(&parser->token, shown, sizeof(shown));
            return parser_error(parser, &parser->token,
                                "expected a parameter name, found %s", shown);
        }
        Token name_token = parser->token;
        uint32_t name = whippet_arena_intern(parser->arena, name_token.start,
                                             name_token.length);
        if (name_map_lookup(&parser->values, name, NULL)) {
            return parser_error(parser, &name_token,
                                "redefinition of value '%%%.*s'",
                                (int)name_token.length, name_token.start);
        }
        if (!parser_advance(parser)) {
            return false;
        }

        WhippetParam parameter;
        memset(&parameter, 0, sizeof(parameter));
        parameter.name = name;
        parameter.type = (uint32_t)type;
        if (!function->is_declaration) {
            uint32_t value_id = 0;
            if (!whippet_function_add_value(function, name, type, 0,
                                            &value_id)) {
                return parser_error(parser, &name_token,
                                    "out of memory while adding parameter");
            }
            parameter.value = value_id;
            name_map_insert(&parser->values, parser->arena, name, value_id);
        } else {
            name_map_insert(&parser->values, parser->arena, name, 0);
        }
        if (!whippet_function_add_param(function, &parameter)) {
            return parser_error(parser, &name_token,
                                "out of memory while adding parameter");
        }

        if (parser->token.kind == TOKEN_COMMA) {
            if (!parser_advance(parser)) {
                return false;
            }
            continue;
        }
        if (parser->token.kind == TOKEN_RPAREN) {
            return parser_advance(parser);
        }
        char shown[64];
        describe_token(&parser->token, shown, sizeof(shown));
        return parser_error(parser, &parser->token,
                            "expected ',' or ')' in parameter list, found %s",
                            shown);
    }
}

static bool finish_block(Parser *parser, const Token *at) {
    if (parser->block == NULL) {
        return true;
    }
    if (!parser->block_terminated) {
        const char *name =
            whippet_arena_string(parser->arena, parser->block->name);
        return parser_error(parser, at,
                            "block '%s' has no terminator instruction", name);
    }
    return true;
}

/* Forward declarations for validation, called once a body has been parsed. */
static bool validate_function(Parser *parser);
static bool parse_instruction(Parser *parser);

static bool parse_function(Parser *parser, bool declaration) {
    if (!parser_advance(parser)) { /* consume 'func' / 'declare' */
        return false;
    }

    WhippetType declared_return = WHIPPET_TYPE_VOID;
    if (declaration) {
        /* declare <return-type> @name(params) */
        if (!parse_type(parser, &declared_return, true)) {
            return false;
        }
    }

    if (parser->token.kind != TOKEN_SYMBOL) {
        char shown[64];
        describe_token(&parser->token, shown, sizeof(shown));
        return parser_error(parser, &parser->token,
                            "expected a function name, found %s", shown);
    }
    Token name_token = parser->token;
    uint32_t name = whippet_arena_intern(parser->arena, name_token.start,
                                         name_token.length);
    if (name_map_lookup(&parser->functions, name, NULL)) {
        return parser_error(parser, &name_token,
                            "duplicate definition of function '@%.*s'",
                            (int)name_token.length, name_token.start);
    }
    if (!parser_advance(parser)) {
        return false;
    }

    name_map_reset(&parser->values);
    name_map_reset(&parser->blocks);

    WhippetFunction *function = whippet_function_create(
        parser->module, name, declared_return, declaration);
    name_map_insert(&parser->functions, parser->arena, name, function->id);
    parser->function = function;

    if (!parse_params(parser, function)) {
        return false;
    }

    if (parser->token.kind == TOKEN_ARROW) {
        if (!parser_advance(parser)) {
            return false;
        }
        WhippetType return_type = WHIPPET_TYPE_VOID;
        if (!parse_type(parser, &return_type, true)) {
            return false;
        }
        function->return_type = (uint8_t)return_type;
    }

    if (declaration) {
        if (parser->token.kind == TOKEN_LBRACE) {
            return parser_error(parser, &parser->token,
                                "a declaration cannot have a function body");
        }
        parser->function = NULL;
        return true;
    }

    if (!expect(parser, TOKEN_LBRACE, "'{'")) {
        return false;
    }

    parser->block = NULL;
    parser->block_terminated = false;
    parser->block_seen_non_phi = false;
    parser->has_block = false;

    while (!parser->failed && parser->token.kind != TOKEN_RBRACE) {
        if (parser->token.kind == TOKEN_EOF) {
            return parser_error(parser, &parser->token,
                                "unexpected end of file inside function '%s'",
                                whippet_arena_string(parser->arena, name));
        }

        if (peek_is_colon(parser)) {
            /* New basic block. */
            if (!finish_block(parser, &parser->token)) {
                return false;
            }
            Token label_token = parser->token;
            uint32_t label = whippet_arena_intern(
                parser->arena, label_token.start, label_token.length);
            if (name_map_lookup(&parser->blocks, label, NULL)) {
                return parser_error(parser, &label_token,
                                    "duplicate block label '%.*s'",
                                    (int)label_token.length,
                                    label_token.start);
            }
            if (!parser_advance(parser)) {
                return false;
            }
            if (!expect(parser, TOKEN_COLON, "':'")) {
                return false;
            }

            WhippetBlock *block = (WhippetBlock *)whippet_arena_alloc(
                parser->arena, sizeof(WhippetBlock), 8);
            memset(block, 0, sizeof(*block));
            block->name = label;
            block->first = WHIPPET_HANDLE_NONE;
            block->last = WHIPPET_HANDLE_NONE;
            if (!whippet_function_add_block(function, block)) {
                return parser_error(parser, &label_token,
                                    "out of memory while adding block");
            }
            name_map_insert(&parser->blocks, parser->arena, label, block->id);
            parser->block = block;
            parser->block_terminated = false;
            parser->block_seen_non_phi = false;
            parser->has_block = true;
            continue;
        }

        if (parser->block == NULL) {
            char shown[64];
            describe_token(&parser->token, shown, sizeof(shown));
            return parser_error(parser, &parser->token,
                                "instruction outside of a basic block: %s",
                                shown);
        }
        if (parser->block_terminated) {
            return parser_error(parser, &parser->token,
                                "instruction after a terminator");
        }

        /* parse_instruction is defined below. */
        if (!parse_instruction(parser)) {
            return false;
        }
    }

    if (!finish_block(parser, &parser->token)) {
        return false;
    }
    if (!parser->has_block) {
        return parser_error(parser, &parser->token,
                            "function '%s' requires at least one basic block",
                            whippet_arena_string(parser->arena, name));
    }
    if (!expect(parser, TOKEN_RBRACE, "'}'")) {
        return false;
    }

    parser->block = NULL;
    if (!validate_function(parser)) {
        return false;
    }
    parser->function = NULL;
    return true;
}

/* ------------------------------------------------------------------ */
/* Instruction parsing                                                 */
/* ------------------------------------------------------------------ */

static bool parse_instruction(Parser *parser) {
    Token result_token;
    memset(&result_token, 0, sizeof(result_token));
    bool has_result = false;
    Token opcode_token;

    if (parser->token.kind == TOKEN_LOCAL) {
        result_token = parser->token;
        has_result = true;
        if (!parser_advance(parser)) {
            return false;
        }
        if (!expect(parser, TOKEN_EQUAL, "'=' after a value name")) {
            return false;
        }
        if (parser->token.kind != TOKEN_IDENT) {
            char shown[64];
            describe_token(&parser->token, shown, sizeof(shown));
            return parser_error(parser, &parser->token,
                                "expected an opcode, found %s", shown);
        }
    } else if (parser->token.kind != TOKEN_IDENT) {
        char shown[64];
        describe_token(&parser->token, shown, sizeof(shown));
        return parser_error(parser, &parser->token,
                            "expected an instruction, found %s", shown);
    }

    opcode_token = parser->token;
    WhippetOpcode opcode;
    if (!whippet_opcode_from_string(opcode_token.start, opcode_token.length,
                                    &opcode)) {
        return parser_error(parser, &opcode_token, "unknown opcode '%.*s'",
                            (int)opcode_token.length, opcode_token.start);
    }
    if (!parser_advance(parser)) {
        return false;
    }

    WhippetOpcodeKind kind = whippet_opcode_kind(opcode);
    bool declares_result = whippet_opcode_has_result(opcode);

    if (has_result && !declares_result) {
        return parser_error(parser, &opcode_token,
                            "opcode '%s' does not produce a result",
                            whippet_opcode_name(opcode));
    }
    if (!has_result && declares_result && opcode != WHIPPET_OP_CALL) {
        return parser_error(parser, &opcode_token,
                            "instruction '%s' requires a result value",
                            whippet_opcode_name(opcode));
    }

    if (opcode == WHIPPET_OP_PHI && parser->block_seen_non_phi) {
        return parser_error(parser, &opcode_token,
                            "phi instructions must appear at the beginning of "
                            "a basic block");
    }
    if (opcode != WHIPPET_OP_PHI) {
        parser->block_seen_non_phi = true;
    }

    uint32_t instruction_handle = whippet_node_alloc(
        parser->module, opcode,
        WHIPPET_LOC_PACK(opcode_token.line, opcode_token.column));
    WhippetInstr *instruction =
        whippet_module_node(parser->module, instruction_handle);
    uint32_t operand_start = parser->module->operand_count;

    WhippetType type = WHIPPET_TYPE_VOID;
    WhippetType second_type = WHIPPET_TYPE_VOID;
    WhippetOperand operand;
    WhippetOperand second;
    WhippetOperand third;

    switch (kind) {
    case WHIPPET_KIND_TERMINATOR:
        switch (opcode) {
        case WHIPPET_OP_RET:
            if (parser->function->return_type == WHIPPET_TYPE_VOID) {
                break;
            }
            if (!parse_type(parser, &type, false)) {
                return false;
            }
            if (type != (WhippetType)parser->function->return_type) {
                return parser_error(
                    parser, &parser->token,
                    "return type '%s' does not match function return type '%s'",
                    whippet_type_name(type),
                    whippet_type_name(
                        (WhippetType)parser->function->return_type));
            }
            if (!parse_typed_operand(parser, type, &operand) ||
                !push_operand(parser, &operand)) {
                return false;
            }
            break;
        case WHIPPET_OP_BR:
            if (!parse_block_operand(parser, &operand)) {
                return false;
            }
            if (!push_operand(parser, &operand)) {
                return false;
            }
            break;
        case WHIPPET_OP_CONDBR:
            if (!parse_type(parser, &type, false)) {
                return false;
            }
            if (!parse_typed_operand(parser, type, &operand)) {
                return false;
            }
            if (!push_operand(parser, &operand) ||
                !expect(parser, TOKEN_COMMA, "','")) {
                return false;
            }
            if (!parse_block_operand(parser, &second)) {
                return false;
            }
            if (!push_operand(parser, &second) ||
                !expect(parser, TOKEN_COMMA, "','")) {
                return false;
            }
            if (!parse_block_operand(parser, &third)) {
                return false;
            }
            if (!push_operand(parser, &third)) {
                return false;
            }
            instruction->result_type = (uint8_t)type;
            break;
        case WHIPPET_OP_SWITCH:
            if (!parse_type(parser, &type, false)) {
                return false;
            }
            if (!parse_typed_operand(parser, type, &operand)) {
                return false;
            }
            if (!push_operand(parser, &operand) ||
                !expect(parser, TOKEN_COMMA, "','") ||
                !expect(parser, TOKEN_LBRACKET, "'['")) {
                return false;
            }
            if (parser->token.kind != TOKEN_RBRACKET) {
                for (;;) {
                    if (!parse_block_operand(parser, &second)) {
                        return false;
                    }
                    if (!push_operand(parser, &second)) {
                        return false;
                    }
                    if (parser->token.kind == TOKEN_COMMA) {
                        if (!parser_advance(parser)) {
                            return false;
                        }
                        continue;
                    }
                    break;
                }
            }
            if (!expect(parser, TOKEN_RBRACKET, "']'") ||
                !expect(parser, TOKEN_COMMA, "','")) {
                return false;
            }
            if (!parse_block_operand(parser, &third)) {
                return false;
            }
            if (!push_operand(parser, &third)) {
                return false;
            }
            instruction->result_type = (uint8_t)type;
            break;
        default:
            return parser_error(parser, &opcode_token, "invalid terminator");
        }
        break;

    case WHIPPET_KIND_BINARY_INT:
    case WHIPPET_KIND_BINARY_FP:
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->result_type = (uint8_t)type;
        if (kind == WHIPPET_KIND_BINARY_INT &&
            !whippet_type_is_integer(type)) {
            return parser_error(parser, &opcode_token,
                                "opcode '%s' requires an integer type, got '%s'",
                                whippet_opcode_name(opcode),
                                whippet_type_name(type));
        }
        if (kind == WHIPPET_KIND_BINARY_FP && !whippet_type_is_float(type)) {
            return parser_error(
                parser, &opcode_token,
                "opcode '%s' requires a floating-point type, got '%s'",
                whippet_opcode_name(opcode), whippet_type_name(type));
        }
        if (!parse_typed_operand(parser, type, &operand) ||
            !push_operand(parser, &operand) ||
            !expect(parser, TOKEN_COMMA, "','")) {
            return false;
        }
        if (!parse_typed_operand(parser, type, &second) ||
            !push_operand(parser, &second)) {
            return false;
        }
        break;

    case WHIPPET_KIND_UNARY_INT:
    case WHIPPET_KIND_UNARY_FP:
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->result_type = (uint8_t)type;
        if (kind == WHIPPET_KIND_UNARY_INT && !whippet_type_is_integer(type)) {
            return parser_error(parser, &opcode_token,
                                "opcode '%s' requires an integer type, got '%s'",
                                whippet_opcode_name(opcode),
                                whippet_type_name(type));
        }
        if (kind == WHIPPET_KIND_UNARY_FP && !whippet_type_is_float(type)) {
            return parser_error(
                parser, &opcode_token,
                "opcode '%s' requires a floating-point type, got '%s'",
                whippet_opcode_name(opcode), whippet_type_name(type));
        }
        if (!parse_typed_operand(parser, type, &operand) ||
            !push_operand(parser, &operand)) {
            return false;
        }
        break;

    case WHIPPET_KIND_ICMP:
    case WHIPPET_KIND_FCMP: {
        if (parser->token.kind != TOKEN_IDENT) {
            char shown[64];
            describe_token(&parser->token, shown, sizeof(shown));
            return parser_error(parser, &parser->token,
                                "expected a comparison predicate, found %s",
                                shown);
        }
        uint8_t predicate = 0;
        if (!whippet_predicate_from_string(opcode, parser->token.start,
                                           parser->token.length, &predicate)) {
            return parser_error(parser, &parser->token,
                                "unknown '%s' predicate '%.*s'",
                                whippet_opcode_name(opcode),
                                (int)parser->token.length,
                                parser->token.start);
        }
        instruction->aux = predicate;
        instruction->result_type = WHIPPET_TYPE_I32;
        if (!parser_advance(parser)) {
            return false;
        }
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->result_type = WHIPPET_TYPE_I32;
        if (!parse_typed_operand(parser, type, &operand) ||
            !push_operand(parser, &operand) ||
            !expect(parser, TOKEN_COMMA, "','")) {
            return false;
        }
        if (!parse_typed_operand(parser, type, &second) ||
            !push_operand(parser, &second)) {
            return false;
        }
        break;
    }

    case WHIPPET_KIND_ALLOCA:
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->aux = (uint8_t)type;
        instruction->result_type = WHIPPET_TYPE_PTR;
        break;

    case WHIPPET_KIND_LOAD:
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->result_type = (uint8_t)type;
        if (!expect(parser, TOKEN_COMMA, "','")) {
            return false;
        }
        if (!parse_type(parser, &second_type, false)) {
            return false;
        }
        if (second_type != WHIPPET_TYPE_PTR) {
            return parser_error(parser, &opcode_token,
                                "load address must have type 'ptr', got '%s'",
                                whippet_type_name(second_type));
        }
        if (!parse_typed_operand(parser, WHIPPET_TYPE_PTR, &operand) ||
            !push_operand(parser, &operand)) {
            return false;
        }
        break;

    case WHIPPET_KIND_STORE:
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->result_type = (uint8_t)type;
        if (!parse_typed_operand(parser, type, &operand) ||
            !push_operand(parser, &operand) ||
            !expect(parser, TOKEN_COMMA, "','")) {
            return false;
        }
        if (!parse_type(parser, &second_type, false)) {
            return false;
        }
        if (second_type != WHIPPET_TYPE_PTR) {
            return parser_error(parser, &opcode_token,
                                "store address must have type 'ptr', got '%s'",
                                whippet_type_name(second_type));
        }
        if (!parse_typed_operand(parser, WHIPPET_TYPE_PTR, &second) ||
            !push_operand(parser, &second)) {
            return false;
        }
        break;

    case WHIPPET_KIND_PTRADD:
        instruction->result_type = WHIPPET_TYPE_PTR;
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        if (type != WHIPPET_TYPE_PTR) {
            return parser_error(parser, &opcode_token,
                                "ptradd base must have type 'ptr', got '%s'",
                                whippet_type_name(type));
        }
        if (!parse_typed_operand(parser, WHIPPET_TYPE_PTR, &operand) ||
            !push_operand(parser, &operand) ||
            !expect(parser, TOKEN_COMMA, "','")) {
            return false;
        }
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        if (!whippet_type_is_integer(type)) {
            return parser_error(parser, &opcode_token,
                                "ptradd offset must be an integer, got '%s'",
                                whippet_type_name(type));
        }
        if (!parse_typed_operand(parser, type, &second) ||
            !push_operand(parser, &second)) {
            return false;
        }
        break;

    case WHIPPET_KIND_PHI: {
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->result_type = (uint8_t)type;
        for (;;) {
            if (!expect(parser, TOKEN_LBRACKET, "'['")) {
                return false;
            }
            if (!parse_typed_operand(parser, type, &operand) ||
                !push_operand(parser, &operand) ||
                !expect(parser, TOKEN_COMMA, "','")) {
                return false;
            }
            if (!parse_block_operand(parser, &second) ||
                !push_operand(parser, &second) ||
                !expect(parser, TOKEN_RBRACKET, "']'")) {
                return false;
            }
            if (parser->token.kind == TOKEN_COMMA) {
                if (!parser_advance(parser)) {
                    return false;
                }
                continue;
            }
            break;
        }
        break;
    }

    case WHIPPET_KIND_COPY:
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->result_type = (uint8_t)type;
        if (!parse_typed_operand(parser, type, &operand) ||
            !push_operand(parser, &operand)) {
            return false;
        }
        break;

    case WHIPPET_KIND_SELECT:
        /* select <cond-type> %c, <value-type> %a, <value-type> %b */
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        if (!parse_typed_operand(parser, type, &operand) ||
            !push_operand(parser, &operand) ||
            !expect(parser, TOKEN_COMMA, "','")) {
            return false;
        }
        if (!parse_type(parser, &second_type, false)) {
            return false;
        }
        if (!parse_typed_operand(parser, second_type, &second) ||
            !push_operand(parser, &second) ||
            !expect(parser, TOKEN_COMMA, "','")) {
            return false;
        }
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        if (!parse_typed_operand(parser, type, &third) ||
            !push_operand(parser, &third)) {
            return false;
        }
        instruction->result_type = (uint8_t)second_type;
        break;

    case WHIPPET_KIND_CALL: {
        if (!parse_type(parser, &type, true)) {
            return false;
        }
        instruction->result_type = (uint8_t)type;
        if (parser->token.kind == TOKEN_SYMBOL) {
            memset(&operand, 0, sizeof(operand));
            operand.kind = WHIPPET_OPERAND_SYMBOL;
            operand.type = WHIPPET_TYPE_VOID;
            operand.name = whippet_arena_intern(
                parser->arena, parser->token.start, parser->token.length);
            parser->last_operand_location = WHIPPET_LOC_PACK(
                parser->token.line, parser->token.column);
            if (!push_operand(parser, &operand) ||
                !parser_advance(parser)) {
                return false;
            }
        } else if (parser->token.kind == TOKEN_LOCAL) {
            if (!parse_typed_operand(parser, WHIPPET_TYPE_PTR, &operand) ||
                !push_operand(parser, &operand)) {
                return false;
            }
        } else {
            char shown[64];
            describe_token(&parser->token, shown, sizeof(shown));
            return parser_error(parser, &parser->token,
                                "expected a callee ('@name' or '%%value'), "
                                "found %s",
                                shown);
        }
        if (!expect(parser, TOKEN_LPAREN, "'('")) {
            return false;
        }
        if (parser->token.kind != TOKEN_RPAREN) {
            for (;;) {
                WhippetType argument_type = WHIPPET_TYPE_VOID;
                if (!parse_type(parser, &argument_type, false)) {
                    return false;
                }
                if (!parse_typed_operand(parser, argument_type, &second) ||
                    !push_operand(parser, &second)) {
                    return false;
                }
                if (parser->token.kind == TOKEN_COMMA) {
                    if (!parser_advance(parser)) {
                        return false;
                    }
                    continue;
                }
                break;
            }
        }
        if (!expect(parser, TOKEN_RPAREN, "')'")) {
            return false;
        }
        break;
    }

    case WHIPPET_KIND_CAST:
        if (!parse_type(parser, &type, false)) {
            return false;
        }
        instruction->aux = (uint8_t)type;
        if (!parse_typed_operand(parser, type, &operand) ||
            !push_operand(parser, &operand)) {
            return false;
        }
        if (!expect_word(parser, "to")) {
            return false;
        }
        if (!parse_type(parser, &second_type, false)) {
            return false;
        }
        instruction->result_type = (uint8_t)second_type;
        break;

    default:
        return parser_error(parser, &opcode_token, "unsupported opcode");
    }

    /* Append to the current block before finalizing the result value. */
    if (parser->block->first == WHIPPET_HANDLE_NONE) {
        parser->block->first = instruction_handle;
    } else {
        WhippetInstr *last =
            whippet_module_node(parser->module, parser->block->last);
        last->next = instruction_handle;
    }
    parser->block->last = instruction_handle;

    uint32_t operand_count = parser->module->operand_count - operand_start;
    if (operand_count > 0xFFFFu) {
        return parser_error(parser, &opcode_token,
                            "too many operands in one instruction");
    }
    instruction->operands = operand_start;
    instruction->operand_count = (uint16_t)operand_count;

    if (has_result) {
        WhippetType result_type = (WhippetType)instruction->result_type;
        if (result_type == WHIPPET_TYPE_VOID) {
            return parser_error(parser, &result_token,
                                "an instruction returning void cannot define a "
                                "value");
        }
        uint32_t name = whippet_arena_intern(
            parser->arena, result_token.start, result_token.length);
        if (name_map_lookup(&parser->values, name, NULL)) {
            return parser_error(parser, &result_token,
                                "redefinition of value '%%%.*s'",
                                (int)result_token.length, result_token.start);
        }
        uint32_t value_id = 0;
        if (!whippet_function_add_value(parser->function, name, result_type,
                                        instruction_handle, &value_id)) {
            return parser_error(parser, &result_token,
                                "out of memory while adding value");
        }
        instruction->result = value_id;
        name_map_insert(&parser->values, parser->arena, name, value_id);
    }

    if (kind == WHIPPET_KIND_TERMINATOR) {
        parser->block_terminated = true;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static bool resolve_operands(Parser *parser, WhippetFunction *function,
                             WhippetInstr *instruction) {
    const WhippetOperand *operands =
        whippet_instr_operands(parser->module, instruction);
    for (uint16_t i = 0; i < instruction->operand_count; ++i) {
        const WhippetOperand *operand = &operands[i];
        uint32_t location =
            parser->module->operand_locations[instruction->operands + i];
        WhippetOperand *mutable_operand =
            &parser->module->operands[instruction->operands + i];
        if (operand->kind == WHIPPET_OPERAND_VALUE) {
            uint32_t value_id = 0;
            if (!name_map_lookup(&parser->values, operand->name, &value_id)) {
                return parser_error_loc(
                    parser, location, "use of undefined value '%%%s'",
                    whippet_arena_string(parser->arena, operand->name));
            }
            mutable_operand->resolved = value_id;
            WhippetType actual =
                (WhippetType)function->values[value_id - 1u].type;
            if (actual != (WhippetType)operand->type) {
                return parser_error_loc(
                    parser, location, "value '%%%s' has type '%s', expected '%s'",
                    whippet_arena_string(parser->arena, operand->name),
                    whippet_type_name(actual),
                    whippet_type_name((WhippetType)operand->type));
            }
        } else if (operand->kind == WHIPPET_OPERAND_BLOCK) {
            uint32_t block_id = 0;
            if (!name_map_lookup(&parser->blocks, operand->name, &block_id)) {
                return parser_error_loc(
                    parser, location, "reference to undefined block '%s'",
                    whippet_arena_string(parser->arena, operand->name));
            }
            mutable_operand->resolved = block_id;
        } else if (operand->kind == WHIPPET_OPERAND_INT) {
            if (whippet_type_is_float((WhippetType)operand->type) ||
                operand->type == WHIPPET_TYPE_PTR) {
                return parser_error_loc(parser, location,
                                        "invalid integer constant type '%s'",
                                        whippet_type_name(
                                            (WhippetType)operand->type));
            }
        } else if (operand->kind == WHIPPET_OPERAND_FLOAT) {
            if (!whippet_type_is_float((WhippetType)operand->type)) {
                return parser_error_loc(parser, location,
                                        "invalid floating constant type '%s'",
                                        whippet_type_name(
                                            (WhippetType)operand->type));
            }
        }
    }
    return true;
}

static bool cast_is_legal(WhippetOpcode opcode, WhippetType source,
                          WhippetType destination) {
    uint32_t source_size = whippet_type_size(source);
    uint32_t destination_size = whippet_type_size(destination);
    switch (opcode) {
    case WHIPPET_OP_TRUNC:
        return whippet_type_is_integer(source) &&
               whippet_type_is_integer(destination) &&
               source_size > destination_size;
    case WHIPPET_OP_ZEXT:
    case WHIPPET_OP_SEXT:
        return whippet_type_is_integer(source) &&
               whippet_type_is_integer(destination) &&
               source_size < destination_size;
    case WHIPPET_OP_FPEXT:
        return source == WHIPPET_TYPE_F32 && destination == WHIPPET_TYPE_F64;
    case WHIPPET_OP_FPTRUNC:
        return source == WHIPPET_TYPE_F64 && destination == WHIPPET_TYPE_F32;
    case WHIPPET_OP_BITCAST:
        return source != WHIPPET_TYPE_VOID && destination != WHIPPET_TYPE_VOID &&
               source_size != 0 && source_size == destination_size;
    case WHIPPET_OP_PTRTOINT:
        return source == WHIPPET_TYPE_PTR &&
               whippet_type_is_integer(destination);
    case WHIPPET_OP_INTTOPTR:
        return whippet_type_is_integer(source) &&
               destination == WHIPPET_TYPE_PTR;
    case WHIPPET_OP_SITOFP:
    case WHIPPET_OP_UITOFP:
        return whippet_type_is_integer(source) &&
               whippet_type_is_float(destination);
    case WHIPPET_OP_FPTOSI:
    case WHIPPET_OP_FPTOUI:
        return whippet_type_is_float(source) &&
               whippet_type_is_integer(destination);
    default:
        return false;
    }
}

static bool validate_instruction(Parser *parser, WhippetFunction *function,
                                 WhippetInstr *instruction) {
    if (!resolve_operands(parser, function, instruction)) {
        return false;
    }
    const WhippetOperand *operands =
        whippet_instr_operands(parser->module, instruction);
    WhippetOpcode opcode = (WhippetOpcode)instruction->opcode;
    WhippetOpcodeKind kind = whippet_opcode_kind(opcode);
    uint32_t location = instruction->location;

    switch (kind) {
    case WHIPPET_KIND_BINARY_INT:
        if (!whippet_type_is_integer(
                (WhippetType)instruction->result_type)) {
            return parser_error_loc(parser, location,
                                    "opcode '%s' requires an integer type",
                                    whippet_opcode_name(opcode));
        }
        if (instruction->operand_count != 2) {
            return parser_error_loc(parser, location,
                                    "opcode '%s' expects two operands",
                                    whippet_opcode_name(opcode));
        }
        break;

    case WHIPPET_KIND_BINARY_FP:
        if (!whippet_type_is_float((WhippetType)instruction->result_type)) {
            return parser_error_loc(
                parser, location, "opcode '%s' requires a floating-point type",
                whippet_opcode_name(opcode));
        }
        if (instruction->operand_count != 2) {
            return parser_error_loc(parser, location,
                                    "opcode '%s' expects two operands",
                                    whippet_opcode_name(opcode));
        }
        break;

    case WHIPPET_KIND_UNARY_INT:
        if (!whippet_type_is_integer(
                (WhippetType)instruction->result_type) ||
            instruction->operand_count != 1) {
            return parser_error_loc(parser, location,
                                    "invalid operands for '%s'",
                                    whippet_opcode_name(opcode));
        }
        break;

    case WHIPPET_KIND_UNARY_FP:
        if (!whippet_type_is_float((WhippetType)instruction->result_type) ||
            instruction->operand_count != 1) {
            return parser_error_loc(parser, location,
                                    "invalid operands for '%s'",
                                    whippet_opcode_name(opcode));
        }
        break;

    case WHIPPET_KIND_ICMP: {
        if (instruction->operand_count != 2) {
            return parser_error_loc(parser, location,
                                    "icmp expects two operands");
        }
        WhippetType operand_type = (WhippetType)operands[0].type;
        if (operand_type != (WhippetType)operands[1].type) {
            return parser_error_loc(parser, location,
                                    "icmp operands have different types");
        }
        if (operand_type == WHIPPET_TYPE_PTR) {
            if (instruction->aux > WHIPPET_ICMP_NE) {
                return parser_error_loc(
                    parser, location,
                    "pointer comparison only supports 'eq' and 'ne'");
            }
        } else if (!whippet_type_is_integer(operand_type)) {
            return parser_error_loc(
                parser, location,
                "icmp requires integer or pointer operands, got '%s'",
                whippet_type_name(operand_type));
        }
        break;
    }

    case WHIPPET_KIND_FCMP: {
        if (instruction->operand_count != 2) {
            return parser_error_loc(parser, location,
                                    "fcmp expects two operands");
        }
        WhippetType operand_type = (WhippetType)operands[0].type;
        if (!whippet_type_is_float(operand_type) ||
            operand_type != (WhippetType)operands[1].type) {
            return parser_error_loc(
                parser, location,
                "fcmp requires matching floating-point operands, got '%s'",
                whippet_type_name(operand_type));
        }
        break;
    }

    case WHIPPET_KIND_ALLOCA:
        if (instruction->result_type != WHIPPET_TYPE_PTR ||
            instruction->aux == WHIPPET_TYPE_VOID) {
            return parser_error_loc(parser, location, "invalid alloca");
        }
        break;

    case WHIPPET_KIND_LOAD:
        if (instruction->operand_count != 1 ||
            operands[0].type != WHIPPET_TYPE_PTR) {
            return parser_error_loc(parser, location,
                                    "load requires a pointer operand");
        }
        break;

    case WHIPPET_KIND_STORE:
        if (instruction->operand_count != 2 ||
            operands[1].type != WHIPPET_TYPE_PTR ||
            operands[0].type != instruction->result_type) {
            return parser_error_loc(parser, location,
                                    "store operand types do not match");
        }
        break;

    case WHIPPET_KIND_PTRADD:
        if (instruction->operand_count != 2 ||
            operands[0].type != WHIPPET_TYPE_PTR ||
            !whippet_type_is_integer((WhippetType)operands[1].type) ||
            instruction->result_type != WHIPPET_TYPE_PTR) {
            return parser_error_loc(parser, location,
                                    "ptradd requires a pointer and an integer");
        }
        break;

    case WHIPPET_KIND_PHI: {
        if (instruction->operand_count < 2 ||
            (instruction->operand_count & 1u) != 0) {
            return parser_error_loc(parser, location,
                                    "phi requires value/block pairs");
        }
        for (uint16_t i = 0; i < instruction->operand_count; i += 2) {
            if (operands[i].kind != WHIPPET_OPERAND_VALUE &&
                operands[i].kind != WHIPPET_OPERAND_INT &&
                operands[i].kind != WHIPPET_OPERAND_FLOAT) {
                return parser_error_loc(parser, location,
                                        "phi incoming value is invalid");
            }
            if (operands[i].type != instruction->result_type) {
                return parser_error_loc(parser, location,
                                        "phi incoming value type mismatch");
            }
            if (operands[i + 1u].kind != WHIPPET_OPERAND_BLOCK ||
                operands[i + 1u].resolved == 0) {
                return parser_error_loc(parser, location,
                                        "phi incoming block is invalid");
            }
        }
        break;
    }

    case WHIPPET_KIND_COPY:
        if (instruction->operand_count != 1 ||
            operands[0].type != instruction->result_type) {
            return parser_error_loc(parser, location,
                                    "copy operand type mismatch");
        }
        break;

    case WHIPPET_KIND_SELECT:
        if (instruction->operand_count != 3 ||
            !whippet_type_is_integer((WhippetType)operands[0].type) ||
            operands[1].type != instruction->result_type ||
            operands[2].type != instruction->result_type ||
            operands[1].type != operands[2].type) {
            return parser_error_loc(parser, location, "invalid select operands");
        }
        break;

    case WHIPPET_KIND_CALL:
        if (instruction->operand_count < 1) {
            return parser_error_loc(parser, location, "call has no callee");
        }
        if (operands[0].kind == WHIPPET_OPERAND_VALUE) {
            if (operands[0].type != WHIPPET_TYPE_PTR) {
                return parser_error_loc(parser, location,
                                        "indirect callee must have type 'ptr'");
            }
        } else if (operands[0].kind != WHIPPET_OPERAND_SYMBOL) {
            return parser_error_loc(parser, location, "invalid call callee");
        }
        break;

    case WHIPPET_KIND_CAST:
        if (instruction->operand_count != 1 ||
            operands[0].type != instruction->aux ||
            !cast_is_legal(opcode, (WhippetType)instruction->aux,
                           (WhippetType)instruction->result_type)) {
            return parser_error_loc(
                parser, location, "illegal '%s' from '%s' to '%s'",
                whippet_opcode_name(opcode),
                whippet_type_name((WhippetType)instruction->aux),
                whippet_type_name((WhippetType)instruction->result_type));
        }
        break;

    case WHIPPET_KIND_TERMINATOR:
        switch (opcode) {
        case WHIPPET_OP_RET:
            if (function->return_type == WHIPPET_TYPE_VOID) {
                if (instruction->operand_count != 0) {
                    return parser_error_loc(
                        parser, location,
                        "void function must use a bare 'ret'");
                }
            } else if (instruction->operand_count != 1 ||
                       operands[0].type != function->return_type) {
                return parser_error_loc(parser, location,
                                        "return value type mismatch");
            }
            break;
        case WHIPPET_OP_BR:
            if (instruction->operand_count != 1 ||
                operands[0].kind != WHIPPET_OPERAND_BLOCK) {
                return parser_error_loc(parser, location,
                                        "br requires one block label");
            }
            break;
        case WHIPPET_OP_CONDBR:
            if (instruction->operand_count != 3 ||
                !whippet_type_is_integer((WhippetType)operands[0].type) ||
                operands[1].kind != WHIPPET_OPERAND_BLOCK ||
                operands[2].kind != WHIPPET_OPERAND_BLOCK) {
                return parser_error_loc(parser, location,
                                        "condbr requires a condition and two "
                                        "block labels");
            }
            break;
        case WHIPPET_OP_SWITCH:
            if (instruction->operand_count < 2 ||
                !whippet_type_is_integer((WhippetType)operands[0].type)) {
                return parser_error_loc(parser, location,
                                        "switch requires an integer tag and a "
                                        "default label");
            }
            for (uint16_t i = 1; i < instruction->operand_count; ++i) {
                if (operands[i].kind != WHIPPET_OPERAND_BLOCK ||
                    operands[i].resolved == 0) {
                    return parser_error_loc(parser, location,
                                            "switch target is invalid");
                }
            }
            break;
        default:
            return parser_error_loc(parser, location, "invalid terminator");
        }
        break;

    default:
        return parser_error_loc(parser, location, "unsupported opcode");
    }
    return true;
}

static bool validate_function(Parser *parser) {
    WhippetFunction *function = parser->function;
    for (WhippetBlock *block = function->first_block; block != NULL;
         block = block->next) {
        WhippetInstr *instruction = NULL;
        for (uint32_t handle = block->first; handle != WHIPPET_HANDLE_NONE;
             handle = instruction->next) {
            instruction = whippet_module_node(parser->module, handle);
            if (!validate_instruction(parser, function, instruction)) {
                return false;
            }
        }
        if (block->first == WHIPPET_HANDLE_NONE) {
            return parser_error_loc(
                parser, WHIPPET_LOC_PACK(1, 1),
                "block '%s' is empty",
                whippet_arena_string(parser->arena, block->name));
        }
        if (whippet_opcode_kind(
                (WhippetOpcode)whippet_module_node(parser->module, block->last)
                    ->opcode) != WHIPPET_KIND_TERMINATOR) {
            return parser_error_loc(
                parser, whippet_module_node(parser->module, block->last)
                            ->location,
                "block '%s' has no terminator instruction",
                whippet_arena_string(parser->arena, block->name));
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Module parsing                                                      */
/* ------------------------------------------------------------------ */

static WhippetFunction *find_function(WhippetModule *module, uint32_t id) {
    for (WhippetFunction *function = module->first_function; function != NULL;
         function = function->next) {
        if (function->id == id) {
            return function;
        }
    }
    return NULL;
}

static bool validate_calls(Parser *parser) {
    WhippetModule *module = parser->module;
    for (WhippetFunction *function = module->first_function; function != NULL;
         function = function->next) {
        for (WhippetBlock *block = function->first_block; block != NULL;
             block = block->next) {
            WhippetInstr *instruction = NULL;
            for (uint32_t handle = block->first;
                 handle != WHIPPET_HANDLE_NONE; handle = instruction->next) {
                instruction = whippet_module_node(module, handle);
                if ((WhippetOpcode)instruction->opcode != WHIPPET_OP_CALL) {
                    continue;
                }
                WhippetOperand *operands =
                    &module->operands[instruction->operands];
                if (operands[0].kind != WHIPPET_OPERAND_SYMBOL) {
                    continue;
                }
                uint32_t target_id = 0;
                if (!name_map_lookup(&parser->functions, operands[0].name,
                                     &target_id)) {
                    return parser_error_loc(
                        parser,
                        module->operand_locations[instruction->operands],
                        "call to undefined function '@%s'",
                        whippet_arena_string(parser->arena,
                                             operands[0].name));
                }
                WhippetFunction *target = find_function(module, target_id);
                if (target == NULL) {
                    return parser_error_loc(
                        parser,
                        module->operand_locations[instruction->operands],
                        "call target '@%s' is missing",
                        whippet_arena_string(parser->arena,
                                             operands[0].name));
                }
                operands[0].resolved = target->id;

                uint32_t argument_count = instruction->operand_count - 1u;
                if (argument_count != target->param_count) {
                    return parser_error_loc(
                        parser, instruction->location,
                        "call to '@%s' passes %u argument(s), expected %u",
                        whippet_arena_string(parser->arena, operands[0].name),
                        argument_count, target->param_count);
                }
                for (uint32_t i = 0; i < argument_count; ++i) {
                    WhippetType argument_type =
                        (WhippetType)operands[i + 1u].type;
                    WhippetType parameter_type =
                        (WhippetType)target->params[i].type;
                    if (argument_type != parameter_type) {
                        return parser_error_loc(
                            parser,
                            module->operand_locations[instruction->operands + i +
                                                      1u],
                            "argument %u of '@%s' has type '%s', expected '%s'",
                            i + 1u,
                            whippet_arena_string(parser->arena,
                                                 operands[0].name),
                            whippet_type_name(argument_type),
                            whippet_type_name(parameter_type));
                    }
                }
                if ((WhippetType)instruction->result_type !=
                    (WhippetType)target->return_type) {
                    return parser_error_loc(
                        parser, instruction->location,
                        "call to '@%s' returns '%s', expected '%s'",
                        whippet_arena_string(parser->arena, operands[0].name),
                        whippet_type_name((WhippetType)target->return_type),
                        whippet_type_name(
                            (WhippetType)instruction->result_type));
                }
            }
        }
    }
    return true;
}

static bool parse_module(Parser *parser) {
    while (!parser->failed && parser->token.kind != TOKEN_EOF) {
        if (token_is_word(&parser->token, "func")) {
            if (!parse_function(parser, false)) {
                return false;
            }
        } else if (token_is_word(&parser->token, "declare")) {
            if (!parse_function(parser, true)) {
                return false;
            }
        } else {
            char shown[64];
            describe_token(&parser->token, shown, sizeof(shown));
            return parser_error(parser, &parser->token,
                                "expected 'func' or 'declare' at module scope, "
                                "found %s",
                                shown);
        }
    }
    if (parser->failed) {
        return false;
    }
    return validate_calls(parser);
}

bool whippet_parse_buffer(WhippetArena *arena, const char *source,
                          size_t length, WhippetModule **module,
                          WhippetDiagnostic *diagnostic) {
    if (module != NULL) {
        *module = NULL;
    }
    if (diagnostic != NULL) {
        memset(diagnostic, 0, sizeof(*diagnostic));
    }

    /* Tolerate a UTF-8 BOM produced by Windows editors. */
    if (length >= 3u && (unsigned char)source[0] == 0xEF &&
        (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) {
        source += 3;
        length -= 3u;
    }

    Parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.arena = arena;
    parser.diagnostic = diagnostic;
    parser.lexer.source = source;
    parser.lexer.length = length;
    parser.lexer.position = 0;
    parser.lexer.line = 1;
    parser.lexer.column = 1;
    parser.module = whippet_module_create(arena);
    name_map_init(&parser.functions, arena);
    name_map_init(&parser.values, arena);
    name_map_init(&parser.blocks, arena);

    if (!parser_advance(&parser)) {
        return false;
    }
    if (!parse_module(&parser)) {
        return false;
    }
    if (module != NULL) {
        *module = parser.module;
    }
    return true;
}

bool whippet_parse_file(WhippetArena *arena, const char *path,
                        WhippetModule **module,
                        WhippetDiagnostic *diagnostic) {
    char *source = NULL;
    size_t length = 0;
    if (!whippet_read_file(arena, path, &source, &length)) {
        if (diagnostic != NULL) {
            memset(diagnostic, 0, sizeof(*diagnostic));
            diagnostic->line = 0;
            diagnostic->column = 0;
            snprintf(diagnostic->message, sizeof(diagnostic->message),
                     "cannot read '%s'", path);
        }
        if (module != NULL) {
            *module = NULL;
        }
        return false;
    }
    return whippet_parse_buffer(arena, source, length, module, diagnostic);
}
