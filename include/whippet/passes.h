/*
 * Author: dhjs0000
 *
 * M3 optimization interface.
 */
#ifndef WHIPPET_PASSES_H
#define WHIPPET_PASSES_H

#include "whippet/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runs the O1 pipeline on a parsed module in place. level 0 is a no-op;
 * level 1 runs constant folding/propagation, copy propagation, DCE and
 * simple strength reduction. Returns false only on an internal failure.
 */
bool whippet_optimize_module(WhippetModule *module, int level,
                             WhippetDiagnostic *diagnostic);

#ifdef __cplusplus
}
#endif

#endif /* WHIPPET_PASSES_H */
