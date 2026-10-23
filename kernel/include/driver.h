#ifndef POCKET_DRIVER_H
#define POCKET_DRIVER_H

#include "vm.h"

/* Desktop bootstrap orchestration only. Compiler decisions (macro expansion,
 * EVAL-WHEN, lowering and image serialization) stay in Lisp. Both hosts use
 * this same file order and reset the same gensym counter before rebuilding. */
extern const char *const p_compiler_files[];
extern const size_t p_compiler_file_count;
bool p_driver_source(PVM *vm, const char *source, size_t length,
                     Value *last, Value *reverse_protos);
bool p_driver_file(PVM *vm, const char *path, Value *last, Value *reverse_protos);
bool p_driver_load_image(PVM *vm, const char *path);
bool p_driver_build_image(PVM *vm, const char *source_root, const char *output);
bool p_driver_exports(Pocket *p);
bool p_driver_print(Pocket *p, Value value);
int p_driver_report(Pocket *p, const char *source);
void p_driver_stats(Pocket *p);

#endif
