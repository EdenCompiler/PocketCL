#include "pocket.h"
#include "test.h"

void test_package(void)
{
    Pocket pocket;
    Value alpha, beta, client, symbol, shadowed;
    int status = -1;
    PPackage *package;
    PPackageEntry *entry;
    Value previous = 0;

    ASSERT(p_init(&pocket, 1024u * 1024u));
    ASSERT(pocket.nil != 0u && p_type(pocket.nil) == P_SYMBOL);
    ASSERT(pocket.truth != 0u && p_type(pocket.truth) == P_SYMBOL);
    ASSERT(p_symbol_ptr(pocket.nil)->value == pocket.nil);
    ASSERT(p_symbol_ptr(pocket.truth)->value == pocket.truth);
    ASSERT((p_symbol_ptr(pocket.nil)->flags & P_CONSTANT) != 0u);
    ASSERT((p_symbol_ptr(pocket.truth)->flags & P_CONSTANT) != 0u);
    ASSERT(p_find_package(&pocket, "CL") == pocket.cl);
    ASSERT(p_find_package(&pocket, "COMMON-LISP") == pocket.cl);
    ASSERT(p_find_package(&pocket, "CL-USER") == pocket.user);
    ASSERT(p_find_package(&pocket, "COMMON-LISP-USER") == pocket.user);
    ASSERT(pocket.current_package == pocket.user);
    {
        Value arbitrary_keyword = p_intern(&pocket, pocket.keyword,
                                           "MixedCase", &status);
        ASSERT(arbitrary_keyword != 0u && status == 0);
        ASSERT(p_symbol_ptr(arbitrary_keyword)->value == arbitrary_keyword);
        ASSERT((p_symbol_ptr(arbitrary_keyword)->flags & P_CONSTANT) != 0u);
        ASSERT(p_find_symbol(&pocket, pocket.keyword, "MixedCase", &status) ==
               arbitrary_keyword);
        ASSERT(status == 2);
    }
    ASSERT(p_find_symbol(&pocket, pocket.cl, "NIL", &status) == pocket.nil);
    ASSERT(status == 2);
    ASSERT(p_find_symbol(&pocket, pocket.user, "T", &status) == pocket.truth);
    ASSERT(status == 3);
    ASSERT(p_find_symbol(&pocket, pocket.user, "MISSING", &status) ==
           pocket.nil);
    ASSERT(status == 0);

    alpha = p_make_package(&pocket, "ALPHA");
    beta = p_make_package(&pocket, "BETA");
    client = p_make_package(&pocket, "CLIENT");
    ASSERT(alpha != 0u && beta != 0u && client != 0u);
    ASSERT(p_make_package(&pocket, "ALPHA") == 0u);
    ASSERT(pocket.error.code == P_ERR_PACKAGE);
    p_clear_error(&pocket);

    symbol = p_intern(&pocket, alpha, "SHARED", &status);
    ASSERT(symbol != 0u && status == 0);
    ASSERT(p_export(&pocket, alpha, symbol));
    ASSERT(p_use_package(&pocket, client, alpha));
    ASSERT(p_find_symbol(&pocket, client, "shared", &status) == pocket.nil);
    ASSERT(status == 0); /* Package APIs preserve exact symbol-name case. */
    ASSERT(p_find_symbol(&pocket, client, "SHARED", &status) == symbol);
    ASSERT(status == 3);
    ASSERT(p_intern(&pocket, client, "SHARED", &status) == symbol);
    ASSERT(status == 3); /* INTERN retains the inherited identity. */

    ASSERT(p_unexport(&pocket, alpha, symbol));
    ASSERT(p_find_symbol(&pocket, client, "SHARED", &status) ==
           pocket.nil);
    ASSERT(status == 0);
    ASSERT(p_export(&pocket, alpha, symbol));
    ASSERT(p_import(&pocket, beta, symbol, false));
    ASSERT(p_find_symbol(&pocket, beta, "SHARED", &status) == symbol);
    ASSERT(status == 1); /* Export status belongs to each package entry. */
    ASSERT(p_export(&pocket, beta, symbol));
    ASSERT(p_find_symbol(&pocket, beta, "SHARED", &status) == symbol);
    ASSERT(status == 2);

    {
        Value local_package = p_make_package(&pocket, "LOCAL-CONFLICT");
        Value external_package = p_make_package(&pocket, "EXTERNAL-CONFLICT");
        Value local = p_intern(&pocket, local_package, "DUPLICATE", &status);
        Value external = p_intern(&pocket, external_package, "DUPLICATE", &status);
        ASSERT(local_package != 0u && external_package != 0u &&
               local != 0u && external != 0u);
        ASSERT(p_export(&pocket, external_package, external));
        ASSERT(!p_use_package(&pocket, local_package, external_package));
        ASSERT(pocket.error.code == P_ERR_PACKAGE);
        p_clear_error(&pocket);
        ASSERT(p_shadow(&pocket, local_package, "DUPLICATE"));
        ASSERT(p_use_package(&pocket, local_package, external_package));
    }

    {
        Value uninterned = p_make_symbol(&pocket, "ADOPTED");
        Value destination = p_make_package(&pocket, "ADOPT-DESTINATION");
        ASSERT(uninterned != 0u && destination != 0u);
        ASSERT(p_symbol_ptr(uninterned)->home == pocket.nil);
        ASSERT(p_import(&pocket, destination, uninterned, false));
        ASSERT(p_symbol_ptr(uninterned)->home == destination);
    }

    shadowed = p_intern(&pocket, client, "SHARED", &status);
    ASSERT(shadowed == symbol && status == 3);
    ASSERT(p_import(&pocket, client, pocket.truth, true));
    ASSERT(p_find_symbol(&pocket, client, "T", &status) == pocket.truth);
    ASSERT(status == 1);
    /* A second used package with the same external name is a conflict unless
     * the client has a local shadowing symbol. */
    {
        Value collision_package = p_make_package(&pocket, "COLLISION");
        Value collision = p_intern(&pocket, collision_package, "SHARED", &status);
        ASSERT(collision_package != 0u && collision != 0u);
        ASSERT(p_unuse_package(&pocket, client, alpha));
        ASSERT(p_export(&pocket, collision_package, collision));
        ASSERT(p_use_package(&pocket, client, alpha));
        ASSERT(!p_use_package(&pocket, client, collision_package));
        ASSERT(pocket.error.code == P_ERR_PACKAGE);
        p_clear_error(&pocket);
        ASSERT(p_shadow(&pocket, client, "SHARED"));
        shadowed = p_find_symbol(&pocket, client, "SHARED", &status);
        ASSERT(shadowed != symbol && shadowed != collision && status == 1);
        ASSERT(p_symbol_ptr(shadowed)->home == client);
        ASSERT(p_use_package(&pocket, client, collision_package));
        ASSERT(p_import(&pocket, client, collision, true));
        ASSERT(p_find_symbol(&pocket, client, "SHARED", &status) == collision);
        ASSERT(status == 1);
    }

    /* Export must not partially publish a name when a user's other used
     * package already exports a distinct symbol with that name. */
    {
        Value gamma = p_make_package(&pocket, "GAMMA");
        Value delta = p_make_package(&pocket, "DELTA");
        Value gamma_symbol = p_intern(&pocket, gamma, "X", &status);
        Value delta_symbol = p_intern(&pocket, delta, "X", &status);
        ASSERT(gamma != 0u && delta != 0u && gamma_symbol != 0u &&
               delta_symbol != 0u);
        ASSERT(p_export(&pocket, delta, delta_symbol));
        ASSERT(p_use_package(&pocket, client, gamma));
        ASSERT(p_use_package(&pocket, client, delta));
        ASSERT(!p_export(&pocket, gamma, gamma_symbol));
        ASSERT(pocket.error.code == P_ERR_PACKAGE);
        ASSERT(p_find_symbol(&pocket, gamma, "X", &status) == gamma_symbol);
        ASSERT(status == 1); /* Failed export leaves the entry internal. */
    }

    /* Entries are a stable, append-only iteration order, even after status
     * changes and imports into a package. */
    package = (PPackage *)alpha;
    for (entry = (PPackageEntry *)package->entries; entry != NULL;
         entry = (PPackageEntry *)entry->next) {
        ASSERT(entry->symbol != 0u);
        if (previous != 0u)
            ASSERT(entry != (PPackageEntry *)previous);
        previous = (Value)entry;
    }
    ASSERT(previous == package->entry_tail);

    ASSERT(p_unuse_package(&pocket, client, beta));
    ASSERT(!p_type(((PPackage *)client)->uses) ||
           p_type(((PPackage *)client)->uses) == P_CONS);
    p_destroy(&pocket);
}
