/* Minimal bootstrap host. By default it submits source to eval0; --compiler
 * loads the Lisp compiler, and --vm submits subsequent source to that compiler
 * and the VM. The larger user-facing CLI and reader arrive at M6. */
#include "boot.h"
#include "host.h"
#include "driver.h"
#include "loader.h"
#include "compiler_prim.h"
#include <stdio.h>
#include <string.h>

#ifndef POCKET_SOURCE_DIR
#define POCKET_SOURCE_DIR "."
#endif

static bool show_result(PState *state, Value result)
{
    char text[16384];
    if (!p_debug_print(state->p, result, text, sizeof(text))) return false;
    puts(text);
    return true;
}

static int report(Pocket *p, const char *source)
{
    char datum[1024];
    fprintf(stderr, "%s:%lu:%lu: %s\n", source, (unsigned long)p->error.line,
            (unsigned long)p->error.column, p->error.message);
    if (p->error.datum && p->error.datum != p->nil &&
        p_debug_print(p, p->error.datum, datum, sizeof(datum)))
        fprintf(stderr, "  datum: %s\n", datum);
    return 1;
}

static bool show_exports(Pocket *p)
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

static int repl(PState *state, PVM *vm, bool compiled)
{
    char line[4096];
    size_t length;
    bool eof;
    for (;;) {
        Value result;
        fputs("pocket> ", stdout);
        fflush(stdout);
        p_clear_error(state->p);
        if (!p_host_console_read_line(state->p, line, sizeof(line), &length, &eof)) {
            report(state->p, "<stdin>");
            continue;
        }
        if (eof) return 0;
        bool ok = compiled ? p_driver_source(vm, line, length, &result, NULL)
                           : p_eval_source(state, line, length, &result);
        if (!ok || !show_result(state, result))
            report(state->p, "<stdin>");
    }
}

int main(int argc, char **argv)
{
    Pocket pocket;
    PState state;
    PVM vm = {0};
    const char *boot_directory = POCKET_SOURCE_DIR "/lisp/boot";
    bool compiler = false, compiled = false;
    bool boot = true, interactive = argc == 1;
    int i, result = 0;
    if (!p_init(&pocket, 16u * 1024u * 1024u)) {
        fprintf(stderr, "unable to initialize PocketCL: %s\n", pocket.error.message);
        return 1;
    }
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--stress") == 0) pocket.gc_stress = true;
        if (strcmp(argv[i], "--no-boot") == 0) boot = false;
        if (strcmp(argv[i], "--boot-dir") == 0 && i + 1 < argc) boot_directory = argv[++i];
        if (strcmp(argv[i], "--compiler") == 0 || strcmp(argv[i], "--vm") == 0 ||
            strcmp(argv[i], "--build-image") == 0) compiler = true;
        if (strcmp(argv[i], "--vm") == 0) compiled = true;
    }
    p_eval_init(&state, &pocket);
    /* The compiler's own expansion/analysis has deeper temporary tree walks
     * than ordinary stage-0 application code. Keep a finite bootstrap bound;
     * compiled application calls use the VM's explicit arena stacks. */
    if (compiler) state.max_call_depth = 512u;
    if (!state.initialized || !p_install_primitives(&state) ||
        !p_install_vm_primitives(&state) || !p_install_fasl_primitives(&state) ||
        !p_install_compiler_primitives(&state) ||
        !p_vm_init(&vm, &state, NULL) || (boot && !p_load_boot(&state, boot_directory))) {
        result = report(&pocket, "<bootstrap>");
        goto done;
    }
    if (compiler) {
        size_t index;
        char path[4096];
        for (index = 0; index < p_compiler_file_count; ++index) {
            int count = snprintf(path, sizeof(path), "%s/lisp/compiler/%s",
                                 POCKET_SOURCE_DIR, p_compiler_files[index]);
            if (count < 0 || (size_t)count >= sizeof(path) || !p_load_source(&state, path, NULL)) {
                result = report(&pocket, path);
                goto done;
            }
        }
        /* Compiled user handlers can exit through VM lexical tokens. Run the
         * condition dispatcher on that same engine rather than crossing an
         * interpreted callback during a pending non-local transfer. Image
         * builds compile this file in their ordinary ordered source pass. */
        if (compiled) {
            int count = snprintf(path, sizeof(path), "%s/30-conditions.lisp", boot_directory);
            if (count < 0 || (size_t)count >= sizeof(path) ||
                !p_driver_file(&vm, path, NULL, NULL)) {
                result = report(&pocket, path);
                goto done;
            }
        }
        pocket.current_package = pocket.user;
        {
            int status;
            Value symbol = p_find_symbol(&pocket, pocket.cl, "*PACKAGE*", &status);
            if (status) p_symbol_ptr(symbol)->value = pocket.user;
        }
    }
    for (i = 1; i < argc && !result; ++i) {
        Value value;
        if (strcmp(argv[i], "--stress") == 0 || strcmp(argv[i], "--no-boot") == 0) continue;
        if (strcmp(argv[i], "--boot-dir") == 0) { ++i; continue; }
        if (strcmp(argv[i], "--compiler") == 0 || strcmp(argv[i], "--vm") == 0) continue;
        if (strcmp(argv[i], "--build-image") == 0) {
            if (++i == argc) { fputs("--build-image requires an output path\n", stderr); result = 1; break; }
            if (!p_driver_build_image(&vm, POCKET_SOURCE_DIR, argv[i])) result = report(&pocket, argv[i]);
            continue;
        }
        if (strcmp(argv[i], "-r") == 0) { interactive = true; continue; }
        if (strcmp(argv[i], "--exports") == 0) {
            if (!show_exports(&pocket)) result = report(&pocket, "<exports>");
            continue;
        }
        if (strcmp(argv[i], "--stats") == 0) {
            /* Collect at this explicit host safe point so the live-byte count
             * measures retained objects rather than recent allocation churn.
             * Repeating this option between workloads helps check tail loops. */
            p_gc(&pocket);
            printf("arena=%lu heap=%lu live=%lu collections=%lu\n",
                   (unsigned long)pocket.arena_size,
                   (unsigned long)pocket.heap_size,
                   (unsigned long)pocket.live_bytes,
                   (unsigned long)pocket.collections);
            continue;
        }
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--eval") == 0) {
            if (++i == argc) { fputs("--eval requires a Lisp form\n", stderr); result = 1; break; }
            p_clear_error(&pocket);
            bool ok;
            ok = compiled ? p_driver_source(&vm, argv[i], strlen(argv[i]), &value, NULL)
                          : p_eval_source(&state, argv[i], strlen(argv[i]), &value);
            if (!ok || !show_result(&state, value))
                result = report(&pocket, "<command-line>");
        } else {
            p_clear_error(&pocket);
            bool ok = compiled ? p_driver_file(&vm, argv[i], &value, NULL)
                               : p_load_source(&state, argv[i], &value);
            if (!ok) result = report(&pocket, argv[i]);
        }
    }
    if (!result && interactive) result = repl(&state, &vm, compiled);
 done:
    p_vm_destroy(&vm);
    p_eval_destroy(&state);
    p_destroy(&pocket);
    return result;
}
