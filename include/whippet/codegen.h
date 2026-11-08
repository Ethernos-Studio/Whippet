/*
 * Author: dhjs0000
 *
 * M1 code generation interface: Whippet IR -> GNU as text (Intel syntax).
 */
#ifndef WHIPPET_CODEGEN_H
#define WHIPPET_CODEGEN_H

#include "whippet/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Emits Windows x64 assembly for every function definition in the module.
 * Declarations are accepted but produce no code. On failure returns false,
 * fills *diagnostic, and writes nothing to *out.
 */
bool whippet_emit_asm(const WhippetModule *module, FILE *out,
                      WhippetDiagnostic *diagnostic);

#ifdef __cplusplus
}
#endif

#endif /* WHIPPET_CODEGEN_H */
