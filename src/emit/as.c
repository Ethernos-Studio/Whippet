/*
 * Whippet -- independent compiler backend.
 * Author: dhjs0000
 */

#include "whippet/emit.h"

/*
 * M0: no machine code yet. The "empty emit" stage prints the canonical IR
 * that the frontend handed us, which is exactly what the M0 acceptance test
 * round-trips. M1 starts with naive x86-64 instruction selection here.
 */
bool whippet_emit_text(const WhippetModule *module, FILE *out) {
    return whippet_print_module_to_file(module, out);
}
