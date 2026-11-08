/*
 * Author: dhjs0000
 *
 * Emit layer. M0 exposes the canonical IR text pass-through; M1 adds GNU as
 * text generation through the isel backend.
 */
#include "whippet/emit.h"

#include "whippet/codegen.h"

#include "backend.h"

bool whippet_emit_text(const WhippetModule *module, FILE *out) {
    return whippet_print_module_to_file(module, out);
}

bool whippet_emit_asm(const WhippetModule *module, FILE *out,
                      WhippetDiagnostic *diagnostic) {
    return whippet_codegen_module_asm(module, out, diagnostic);
}
