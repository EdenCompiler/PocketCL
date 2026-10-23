/* Native bytecode host. This executable deliberately has no eval0 link: the
 * compiler is loaded from PFAS and source compilation is ordinary VM calls. */
#include "driver.h"
#include "exec.h"
#include "host.h"
#include "loader.h"
#include "compiler_prim.h"
#include <stdio.h>
#include <string.h>

#ifndef POCKET_SOURCE_DIR
#define POCKET_SOURCE_DIR "."
#endif
#ifndef POCKET_BUILD_DIR
#define POCKET_BUILD_DIR "build/default"
#endif

static int repl(PVM *vm)
{
    Pocket *p = vm->state->p;
    char line[4096];
    size_t length;
    bool eof;
    for (;;) {
        Value result;
        fputs("pocket> ", stdout);
        fflush(stdout);
        p_clear_error(p);
        if (!p_host_console_read_line(p, line, sizeof(line), &length, &eof)) {
            p_driver_report(p, "<stdin>");
            continue;
        }
        if (eof) return 0;
        if (!p_driver_source(vm, line, length, &result, NULL) || !p_driver_print(p, result))
            p_driver_report(p, "<stdin>");
    }
}

int main(int argc, char **argv)
{
    Pocket pocket;
    PState state;
    PVM vm = {0};
    const char *image = NULL;
    char default_image[4096];
    bool interactive = argc == 1, stress = false;
    size_t arena = 16u * 1024u * 1024u;
    int i, result = 0;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) image = argv[++i];
        else if (strcmp(argv[i], "--runtime") == 0) arena = 4u * 1024u * 1024u;
        else if (strcmp(argv[i], "--stress") == 0) stress = true;
    }
    if (!image) {
        int length = POCKET_BUILD_DIR[0] == '/'
            ? snprintf(default_image, sizeof(default_image), "%s/stage1/compiler.fasl", POCKET_BUILD_DIR)
            : snprintf(default_image, sizeof(default_image), "%s/%s/stage1/compiler.fasl",
                       POCKET_SOURCE_DIR, POCKET_BUILD_DIR);
        if (length < 0 || (size_t)length >= sizeof(default_image)) {
            fputs("default compiler image path is too long; use --image PATH\n", stderr);
            return 1;
        }
        image = default_image;
    }
    if (!p_init(&pocket, arena)) {
        fprintf(stderr, "unable to initialize PocketCL: %s\n", pocket.error.message);
        return 1;
    }
    /* A command-line flag may enable stress, but must preserve the default
     * selected by POCKET_GC_STRESS in sanitizer/stress build configurations. */
    if (stress) pocket.gc_stress = true;
    p_state_init(&state, &pocket);
    if (!state.initialized || !p_install_primitives(&state) ||
        !p_install_vm_primitives(&state) || !p_install_fasl_primitives(&state) ||
        !p_install_compiler_primitives(&state) ||
        !p_vm_init(&vm, &state, NULL) || !p_driver_load_image(&vm, image)) {
        result = p_driver_report(&pocket, image);
        goto done;
    }
    for (i = 1; i < argc && !result; ++i) {
        Value value;
        if (strcmp(argv[i], "--image") == 0) { ++i; continue; }
        if (strcmp(argv[i], "--runtime") == 0 || strcmp(argv[i], "--stress") == 0) continue;
        if (strcmp(argv[i], "-r") == 0) { interactive = true; continue; }
        if (strcmp(argv[i], "--exports") == 0) {
            if (!p_driver_exports(&pocket)) result = p_driver_report(&pocket, "<exports>");
            continue;
        }
        if (strcmp(argv[i], "--stats") == 0) { p_driver_stats(&pocket); continue; }
        if (strcmp(argv[i], "--build-image") == 0) {
            if (++i == argc) { fputs("--build-image requires an output path\n", stderr); result = 1; break; }
            if (!p_driver_build_image(&vm, POCKET_SOURCE_DIR, argv[i]))
                result = p_driver_report(&pocket, argv[i]);
            continue;
        }
        p_clear_error(&pocket);
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--eval") == 0) {
            if (++i == argc) { fputs("--eval requires a Lisp form\n", stderr); result = 1; break; }
            if (!p_driver_source(&vm, argv[i], strlen(argv[i]), &value, NULL) ||
                !p_driver_print(&pocket, value)) result = p_driver_report(&pocket, "<command-line>");
        } else if (!p_driver_file(&vm, argv[i], &value, NULL)) {
            result = p_driver_report(&pocket, argv[i]);
        }
    }
    if (!result && interactive) result = repl(&vm);
 done:
    p_vm_destroy(&vm);
    p_state_destroy(&state);
    p_destroy(&pocket);
    return result;
}
