/*
 * Author: dhjs0000
 *
 * Emit interface for the M0 canonical IR pass-through. M1 assembly emission
 * is exposed separately by whippet/codegen.h.
 */
#ifndef WHIPPET_EMIT_H
#define WHIPPET_EMIT_H

#include "whippet/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

bool whippet_emit_text(const WhippetModule *module, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* WHIPPET_EMIT_H */
