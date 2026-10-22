#ifndef POCKET_BOOT_H
#define POCKET_BOOT_H
#include "eval.h"
/* Source loading belongs to the desktop bootstrap host, not the runtime VM. */
bool p_eval_source(PState *state, const char *source, size_t length, Value *last);
bool p_load_source(PState *state, const char *path, Value *last);
bool p_load_boot(PState *state, const char *directory);
#endif
