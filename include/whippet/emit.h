/*
 * Author: dhjs0000
 *
 * Emit interface.
 *
 * M0 only has the textual pass-through: the "emitted" form of a module is
 * its canonical IR text. M1 replaces this with x86-64 GNU as output.
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
