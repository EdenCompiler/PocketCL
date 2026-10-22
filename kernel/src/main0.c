/* Minimal stage-0 host. The complete user-facing CLI and REPL are Lisp/M6.
 * This entry point only loads boot sources and submits forms to eval0. */
#include "boot.h"
#include "host.h"
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

static int repl(PState *state)
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
        if (!p_eval_source(state, line, length, &result) || !show_result(state, result))
            report(state->p, "<stdin>");
    }
}

int main(int argc, char **argv)
{
    Pocket pocket;
    PState state;
    const char *boot_directory = POCKET_SOURCE_DIR "/lisp/boot";
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
    }
    p_eval_init(&state, &pocket);
    if (!state.initialized || !p_install_primitives(&state) || (boot && !p_load_boot(&state, boot_directory))) {
        result = report(&pocket, "<bootstrap>");
        goto done;
    }
    for (i = 1; i < argc && !result; ++i) {
        Value value;
        if (strcmp(argv[i], "--stress") == 0 || strcmp(argv[i], "--no-boot") == 0) continue;
        if (strcmp(argv[i], "--boot-dir") == 0) { ++i; continue; }
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
            if (!p_eval_source(&state, argv[i], strlen(argv[i]), &value) || !show_result(&state, value))
                result = report(&pocket, "<command-line>");
        } else {
            p_clear_error(&pocket);
            if (!p_load_source(&state, argv[i], &value)) result = report(&pocket, argv[i]);
        }
    }
    if (!result && interactive) result = repl(&state);
 done:
    p_eval_destroy(&state);
    p_destroy(&pocket);
    return result;
}
