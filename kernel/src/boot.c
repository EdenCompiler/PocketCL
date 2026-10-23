#include "boot.h"
#include "host.h"
#include <stdio.h>
#include <string.h>

bool p_eval_source(PState *state, const char *source, size_t length, Value *last)
{
    Pocket *p = state->p;
    PReader reader;
    Value roots[2] = {p->nil, p->nil};
    size_t saved = p_root_save(p);
    bool success = true;
    if (!p_roots(p, roots, 2)) return false;
    p_reader_init(&reader, p, source, length);
    for (;;) {
        size_t line = reader.line, column = reader.column;
        roots[0] = p_read(&reader);
        if (!roots[0]) { success = false; break; }
        if (roots[0] == P_EOF) break;
        roots[1] = p_eval0(state, roots[0], 0);
        if (!roots[1] || p->error.code != P_OK) {
            if (!p->error.line) { p->error.line = line; p->error.column = column; }
            success = false;
            break;
        }
    }
    if (last) *last = roots[1];
    p_root_restore(p, saved);
    return success;
}

bool p_load_source(PState *state, const char *path, Value *last)
{
    Pocket *p = state->p;
    Value roots[2] = {p->nil, p->nil};
    size_t saved = p_root_save(p), length;
    bool success = false;
    if (!p_roots(p, roots, 2)) return false;
    if (!p_reserve(p, sizeof(PString) + strlen(path) * sizeof(uint32_t))) goto done;
    roots[0] = p_string_utf8(p, path, strlen(path));
    if (!roots[0] || !p_host_file_size(p, roots[0], &length)) goto done;
    if (length > SIZE_MAX - sizeof(PBytes) || !p_reserve(p, sizeof(PBytes) + length)) goto done;
    roots[1] = p_bytes(p, length);
    if (!roots[1] || !p_host_read_file(p, roots[0], roots[1])) goto done;
    success = p_eval_source(state, (const char *)((PBytes *)roots[1])->data,
                            ((PBytes *)roots[1])->length, last);
 done:
    p_root_restore(p, saved);
    return success;
}

bool p_load_boot(PState *state, const char *directory)
{
    const char *files[] = {"00-core.lisp", "10-lambda-list.lisp", "20-cl-macros.lisp", "30-conditions.lisp"};
    char path[4096];
    size_t i;
    state->p->current_package = state->p->cl;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        int length = snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            p_error(state->p, P_ERR_IO, "bootstrap directory path is too long");
            return false;
        }
        if (!p_load_source(state, path, NULL)) {
            fprintf(stderr, "%s:%lu:%lu: %s\n", path,
                    (unsigned long)state->p->error.line,
                    (unsigned long)state->p->error.column, state->p->error.message);
            return false;
        }
    }
    state->p->current_package = state->p->user;
    {
        int status;
        Value package_symbol = p_find_symbol(state->p, state->p->cl, "*PACKAGE*", &status);
        if (status) p_symbol_ptr(package_symbol)->value = state->p->user;
    }
    return true;
}
