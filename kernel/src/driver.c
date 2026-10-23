#include "driver.h"
#include "host.h"
#include "loader.h"
#include <stdio.h>
#include <string.h>

const char *const p_compiler_files[] = {
    "00-package.lisp", "01-tables.lisp", "10-environment.lisp",
    "20-expand-analyze.lisp", "30-codegen.lisp", "40-driver.lisp", "50-fasl.lisp"
};
const size_t p_compiler_file_count = sizeof(p_compiler_files) / sizeof(p_compiler_files[0]);

static Value read_file(Pocket *p, const char *path)
{
    Value roots[2] = {p->nil, p->nil};
    size_t saved = p_root_save(p), length;
    bool ok = false;
    if (!p_roots(p, roots, 2)) return 0;
    if (strlen(path) > (SIZE_MAX - sizeof(PString)) / sizeof(uint32_t) ||
        !p_reserve(p, sizeof(PString) + strlen(path) * sizeof(uint32_t))) goto done;
    roots[0] = p_string_utf8(p, path, strlen(path));
    if (!roots[0] || !p_host_file_size(p, roots[0], &length)) goto done;
    if (length > SIZE_MAX - sizeof(PBytes) || !p_reserve(p, sizeof(PBytes) + length)) goto done;
    roots[1] = p_bytes(p, length);
    ok = roots[1] && p_host_read_file(p, roots[0], roots[1]);
 done:
    p_root_restore(p, saved);
    return ok ? roots[1] : 0;
}

static bool set_package(Pocket *p, Value package)
{
    int status;
    Value symbol = p_find_symbol(p, p->cl, "*PACKAGE*", &status);
    if (!status) return p_error(p, P_ERR_UNBOUND, "missing *PACKAGE* binding") != 0;
    p->current_package = package;
    p_symbol_ptr(symbol)->value = package;
    return true;
}

static Value compiler_function(Pocket *p, const char *name)
{
    int status;
    Value package = p_find_package(p, "POCKET-COMPILER");
    Value symbol = package ? p_find_symbol(p, package, name, &status) : 0;
    if (!symbol || !status || p_symbol_ptr(symbol)->function == P_UNBOUND)
        return p_error(p, P_ERR_UNDEFINED, "compiler image is missing an entry point");
    return p_symbol_ptr(symbol)->function;
}

bool p_driver_source(PVM *vm, const char *source, size_t length,
                     Value *last, Value *reverse_protos)
{
    PState *state = vm->state;
    Pocket *p = state->p;
    PReader reader;
    Value roots[3] = {p->nil, p->nil, p->nil};
    size_t saved = p_root_save(p);
    bool ok = false;
    if (!p_roots(p, roots, 3)) return false;
    p_reader_init(&reader, p, source, length);
    for (;;) {
        size_t line = reader.line, column = reader.column;
        roots[0] = p_read(&reader);
        if (!roots[0]) goto done;
        if (roots[0] == P_EOF) { ok = true; break; }
        roots[1] = compiler_function(p, "PROCESS-TOPLEVEL");
        if (!roots[1]) goto done;
        roots[1] = p_vm_call(vm, roots[1], 1, roots);
        if (!roots[1] || p->error.code != P_OK) {
            if (!p->error.line) { p->error.line = line; p->error.column = column; }
            goto done;
        }
        roots[2] = state->mv_count > 1 ? state->mv[1] : p->nil;
        if (reverse_protos && roots[1] != p->nil) {
            if (!p_reserve(p, sizeof(PCons))) goto done;
            *reverse_protos = p_cons(p, roots[1], *reverse_protos);
            if (!*reverse_protos) goto done;
        }
    }
    if (last) *last = roots[2];
 done:
    p_root_restore(p, saved);
    return ok;
}

bool p_driver_file(PVM *vm, const char *path, Value *last, Value *reverse_protos)
{
    Pocket *p = vm->state->p;
    Value bytes = p->nil;
    size_t saved = p_root_save(p);
    bool ok = false;
    if (!p_roots(p, &bytes, 1)) return false;
    bytes = read_file(p, path);
    if (bytes)
        ok = p_driver_source(vm, (const char *)((PBytes *)bytes)->data,
                             ((PBytes *)bytes)->length, last, reverse_protos);
    p_root_restore(p, saved);
    return ok;
}

bool p_driver_load_image(PVM *vm, const char *path)
{
    Pocket *p = vm->state->p;
    Value roots[2] = {p->nil, p->nil};
    size_t saved = p_root_save(p), required;
    bool ok = false;
    if (!p_roots(p, roots, 2)) return false;
    roots[0] = read_file(p, path);
    if (!roots[0] || !p_fasl_required_bytes(p, roots[0], &required) ||
        !p_reserve(p, required)) goto done;
    roots[1] = p_fasl_load(p, roots[0]);
    if (!roots[1]) goto done;
    ok = p_vm_call(vm, roots[1], 0, NULL) != 0 && p->error.code == P_OK;
    if (ok) ok = set_package(p, p->user);
 done:
    p_root_restore(p, saved);
    return ok;
}

static bool source_path(Pocket *p, char *path, size_t capacity,
                        const char *root, const char *directory, const char *file)
{
    int count = snprintf(path, capacity, "%s/lisp/%s/%s", root, directory, file);
    if (count < 0 || (size_t)count >= capacity) {
        (void)p_error(p, P_ERR_IO, "source directory path is too long");
        return false;
    }
    return true;
}

bool p_driver_build_image(PVM *vm, const char *source_root, const char *output)
{
    const char *const boot_files[] = {
        "00-core.lisp", "10-lambda-list.lisp", "20-cl-macros.lisp", "30-conditions.lisp"
    };
    Pocket *p = vm->state->p;
    Value roots[4] = {p->nil, p->nil, p->nil, p->nil};
    char path[4096];
    size_t i, saved = p_root_save(p);
    int status;
    bool ok = false;
    Value counter = p_find_symbol(p, p->cl, "*GENSYM-COUNTER*", &status);
    if (!counter || !status) return p_error(p, P_ERR_UNBOUND, "missing *GENSYM-COUNTER*") != 0;
    p_symbol_ptr(counter)->value = p_fixnum(0);
    if (!p_roots(p, roots, 4) || !set_package(p, p->cl)) goto done;
    /* The host chooses files; Lisp PROCESS-TOPLEVEL chooses compile/load
     * effects and returns the initializer proto (or NIL) to retain. */
    for (i = 0; i < sizeof(boot_files) / sizeof(boot_files[0]); ++i) {
        if (!source_path(p, path, sizeof(path), source_root, "boot", boot_files[i])) goto done;
        fprintf(stderr, "Compiling %s\n", path);
        if (!p_driver_file(vm, path, NULL, roots)) goto done;
    }
    for (i = 0; i < p_compiler_file_count; ++i) {
        if (!source_path(p, path, sizeof(path), source_root, "compiler", p_compiler_files[i])) goto done;
        fprintf(stderr, "Compiling %s\n", path);
        if (!p_driver_file(vm, path, NULL, roots)) goto done;
    }
    /* Reverse the host's rooted cons list in place to restore source order. */
    while (roots[0] != p->nil) {
        Value next = p_cons_ptr(roots[0])->cdr;
        p_cons_ptr(roots[0])->cdr = roots[1];
        roots[1] = roots[0];
        roots[0] = next;
    }
    roots[2] = compiler_function(p, "WRITE-FASL");
    roots[0] = p_fixnum(P_FASL_COMPILER);
    {
        Value arguments[2] = {roots[1], roots[0]};
        if (!roots[2]) goto done;
        roots[2] = p_vm_call(vm, roots[2], 2, arguments);
    }
    if (!roots[2] || p->error.code != P_OK) goto done;
    if (strlen(output) > (SIZE_MAX - sizeof(PString)) / sizeof(uint32_t) ||
        !p_reserve(p, sizeof(PString) + strlen(output) * sizeof(uint32_t))) goto done;
    roots[3] = p_string_utf8(p, output, strlen(output));
    ok = roots[3] && p_host_write_file(p, roots[3], roots[2]);
 done:
    p_root_restore(p, saved);
    return ok;
}

bool p_driver_print(Pocket *p, Value value)
{
    char text[16384];
    if (!p_debug_print(p, value, text, sizeof(text))) return false;
    puts(text);
    return true;
}

int p_driver_report(Pocket *p, const char *source)
{
    char datum[1024];
    fprintf(stderr, "%s:%lu:%lu: %s\n", source, (unsigned long)p->error.line,
            (unsigned long)p->error.column, p->error.message);
    if (p->error.datum && p->error.datum != p->nil &&
        p_debug_print(p, p->error.datum, datum, sizeof(datum)))
        fprintf(stderr, "  datum: %s\n", datum);
    return 1;
}

bool p_driver_exports(Pocket *p)
{
    PPackageEntry *entry;
    char name[256];
    size_t length;
    for (entry = (PPackageEntry *)((PPackage *)p->cl)->entries;
         entry; entry = (PPackageEntry *)entry->next) {
        PSymbol *symbol = p_symbol_ptr(entry->symbol);
        const char *kind = "symbol";
        if (!entry->external) continue;
        if (symbol->flags & P_SPECIAL_OPERATOR) kind = "special-operator";
        else if (symbol->flags & P_MACRO) kind = "macro";
        else if (symbol->function != P_UNBOUND) kind = "function";
        else if (symbol->flags & P_CONSTANT) kind = "constant";
        else if (symbol->flags & P_SPECIAL) kind = "variable";
        if (!p_string_to_utf8(p, symbol->name, name, sizeof(name) - 1, &length)) return false;
        name[length] = '\0';
        printf("%s\t%s\n", name, kind);
    }
    return true;
}

void p_driver_stats(Pocket *p)
{
    p_gc(p);
    printf("arena=%lu heap=%lu live=%lu collections=%lu\n",
           (unsigned long)p->arena_size, (unsigned long)p->heap_size,
           (unsigned long)p->live_bytes, (unsigned long)p->collections);
}
