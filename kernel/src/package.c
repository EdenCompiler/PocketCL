#include "pocket.h"

#include <string.h>

/* Package and symbol names compare exactly, including case and Unicode.
 * Only the reader folds unescaped source text; doing it here would merge
 * distinct symbols such as FOO and |foo|. This decoder never allocates. */
static bool utf8_next(const unsigned char **cursor, uint32_t *codepoint)
{
    const unsigned char *p = *cursor;
    unsigned char a = p[0];
    if (a < 0x80u) {
        if (a == 0u)
            return false;
        *cursor = p + 1;
        *codepoint = a;
        return true;
    }
    if (a >= 0xc2u && a <= 0xdfu && p[1] != 0u && (p[1] & 0xc0u) == 0x80u) {
        *codepoint = ((uint32_t)(a & 0x1fu) << 6) | (uint32_t)(p[1] & 0x3fu);
        *cursor = p + 2;
        return true;
    }
    if (a >= 0xe0u && a <= 0xefu && p[1] != 0u && p[2] != 0u &&
        (p[1] & 0xc0u) == 0x80u && (p[2] & 0xc0u) == 0x80u &&
        !(a == 0xe0u && p[1] < 0xa0u) && !(a == 0xedu && p[1] >= 0xa0u)) {
        *codepoint = ((uint32_t)(a & 0x0fu) << 12) |
                     ((uint32_t)(p[1] & 0x3fu) << 6) | (uint32_t)(p[2] & 0x3fu);
        *cursor = p + 3;
        return true;
    }
    if (a >= 0xf0u && a <= 0xf4u && p[1] != 0u && p[2] != 0u && p[3] != 0u &&
        (p[1] & 0xc0u) == 0x80u && (p[2] & 0xc0u) == 0x80u &&
        (p[3] & 0xc0u) == 0x80u && !(a == 0xf0u && p[1] < 0x90u) &&
        !(a == 0xf4u && p[1] >= 0x90u)) {
        *codepoint = ((uint32_t)(a & 7u) << 18) |
                     ((uint32_t)(p[1] & 0x3fu) << 12) |
                     ((uint32_t)(p[2] & 0x3fu) << 6) | (uint32_t)(p[3] & 0x3fu);
        *cursor = p + 4;
        return true;
    }
    return false;
}

static bool name_equal(Value value, const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;
    size_t i = 0;
    PString *string;
    if (p_type(value) != P_STRING || name == NULL)
        return false;
    string = p_string_ptr(value);
    while (*cursor != 0u) {
        uint32_t codepoint;
        if (i >= string->length)
            return false;
        if (!utf8_next(&cursor, &codepoint))
            return false;
        if (string->data[i++] != codepoint)
            return false;
    }
    return i == string->length;
}

static bool names_equal(Value a, Value b)
{
    PString *left, *right;
    size_t i;
    if (p_type(a) != P_STRING || p_type(b) != P_STRING)
        return false;
    left = p_string_ptr(a);
    right = p_string_ptr(b);
    if (left->length != right->length)
        return false;
    for (i = 0; i < left->length; ++i) {
        if (left->data[i] != right->data[i])
            return false;
    }
    return true;
}

static Value make_name(Pocket *p, const char *name)
{
    return p_string_utf8(p, name, strlen(name));
}

static PPackage *package_ptr(Pocket *p, Value package)
{
    if (p_type(package) != P_PACKAGE) {
        p_error(p, P_ERR_TYPE, "expected package");
        return NULL;
    }
    return (PPackage *)package;
}

static PPackageEntry *entry_find(PPackage *package, Value symbol)
{
    PPackageEntry *entry = (PPackageEntry *)package->entries;
    while (entry != NULL) {
        if (entry->symbol == symbol)
            return entry;
        entry = (PPackageEntry *)entry->next;
    }
    return NULL;
}

static PPackageEntry *entry_find_name(PPackage *package, const char *name)
{
    PPackageEntry *entry = (PPackageEntry *)package->entries;
    while (entry != NULL) {
        if (name_equal(p_symbol_ptr(entry->symbol)->name, name))
            return entry;
        entry = (PPackageEntry *)entry->next;
    }
    return NULL;
}

static bool package_accessible_symbol(PPackage *package, Value symbol)
{
    PPackageEntry *entry;
    PCons *use_cell;
    for (entry = (PPackageEntry *)package->entries; entry != NULL;
         entry = (PPackageEntry *)entry->next)
        if (entry->symbol == symbol)
            return true;
    for (use_cell = (PCons *)package->uses;
         p_type((Value)use_cell) == P_CONS;
         use_cell = (PCons *)use_cell->cdr) {
        PPackage *used = (PPackage *)use_cell->car;
        for (entry = (PPackageEntry *)used->entries; entry != NULL;
             entry = (PPackageEntry *)entry->next)
            if (entry->external && entry->symbol == symbol)
                return true;
    }
    return false;
}

static bool list_has(Value list, Value item)
{
    while (p_type(list) == P_CONS) {
        PCons *cell = p_cons_ptr(list);
        if (cell->car == item)
            return true;
        list = cell->cdr;
    }
    return false;
}

static bool add_entry(Pocket *p, PPackage *package, Value symbol, bool external)
{
    PPackageEntry *entry;
    if (entry_find(package, symbol) != NULL)
        return true;
    entry = p_alloc(p, P_PACKAGE_ENTRY, sizeof(*entry));
    if (entry == NULL)
        return false;
    entry->symbol = symbol;
    entry->next = 0;
    entry->external = external;
    if (package->entry_tail == 0)
        package->entries = (Value)entry;
    else
        ((PPackageEntry *)package->entry_tail)->next = (Value)entry;
    package->entry_tail = (Value)entry;
    return true;
}

static bool package_shadows_name(PPackage *package, Value name)
{
    Value cursor = package->shadows;
    while (p_type(cursor) == P_CONS) {
        Value symbol = p_cons_ptr(cursor)->car;
        if (p_type(symbol) == P_SYMBOL &&
            names_equal(p_symbol_ptr(symbol)->name, name))
            return true;
        cursor = p_cons_ptr(cursor)->cdr;
    }
    return false;
}

static bool add_list_value(Pocket *p, Value *list, Value value)
{
    Value cell;
    if (list_has(*list, value))
        return true;
    cell = p_cons(p, value, *list);
    if (cell == 0)
        return false;
    *list = cell;
    return true;
}

static bool local_or_inherited(Pocket *p, PPackage *package, const char *name,
                               Value *found, int *status)
{
    PPackageEntry *local = entry_find_name(package, name);
    Value candidate = 0;
    int candidate_status = 0;
    PCons *use_cell;
    if (local != NULL) {
        *found = local->symbol;
        *status = local->external ? 2 : 1;
        return true;
    }
    for (use_cell = (PCons *)package->uses;
         p_type((Value)use_cell) == P_CONS;
         use_cell = (PCons *)use_cell->cdr) {
        PPackage *used = (PPackage *)use_cell->car;
        PPackageEntry *entry;
        for (entry = (PPackageEntry *)used->entries; entry != NULL;
             entry = (PPackageEntry *)entry->next) {
            if (entry->external && name_equal(p_symbol_ptr(entry->symbol)->name,
                                              name)) {
                if (candidate != 0 && candidate != entry->symbol) {
                    p_error(p, P_ERR_PACKAGE,
                            "package has conflicting inherited symbols");
                    return false;
                }
                candidate = entry->symbol;
                candidate_status = 3;
            }
        }
    }
    *found = candidate;
    *status = candidate_status;
    return true;
}

Value p_make_symbol(Pocket *p, const char *name)
{
    Value text;
    PSymbol *symbol;
    if (name == NULL) {
        p_error(p, P_ERR_TYPE, "symbol name is null");
        return 0;
    }
    text = make_name(p, name);
    if (text == 0)
        return 0;
    symbol = p_alloc(p, P_SYMBOL, sizeof(*symbol));
    if (symbol == NULL)
        return 0;
    symbol->name = text;
    symbol->home = p->nil;
    symbol->value = P_UNBOUND;
    symbol->function = P_UNBOUND;
    symbol->plist = p->nil;
    symbol->flags = 0;
    return (Value)symbol;
}

Value p_make_package(Pocket *p, const char *name)
{
    Value text;
    PPackage *package;
    if (name == NULL || p_find_package(p, name) != 0) {
        if (name == NULL)
            p_error(p, P_ERR_TYPE, "package name is null");
        else
            p_error(p, P_ERR_PACKAGE, "package already exists");
        return 0;
    }
    text = make_name(p, name);
    if (text == 0)
        return 0;
    package = p_alloc(p, P_PACKAGE, sizeof(*package));
    if (package == NULL)
        return 0;
    package->name = text;
    package->nicknames = p->nil;
    package->entries = 0;
    package->entry_tail = 0;
    package->uses = p->nil;
    package->shadows = p->nil;
    package->next = 0;
    if (p->package_tail == 0)
        p->packages = (Value)package;
    else
        ((PPackage *)p->package_tail)->next = (Value)package;
    p->package_tail = (Value)package;
    return (Value)package;
}

Value p_find_package(Pocket *p, const char *name)
{
    PPackage *package = (PPackage *)p->packages;
    if (name == NULL)
        return 0;
    while (package != NULL) {
        Value nicknames = package->nicknames;
        if (name_equal(package->name, name))
            return (Value)package;
        while (p_type(nicknames) == P_CONS) {
            PCons *cell = p_cons_ptr(nicknames);
            if (name_equal(cell->car, name))
                return (Value)package;
            nicknames = cell->cdr;
        }
        package = (PPackage *)package->next;
    }
    return 0;
}

Value p_find_symbol(Pocket *p, Value package_value, const char *name,
                    int *status)
{
    PPackage *package = package_ptr(p, package_value);
    Value found = 0;
    int found_status = 0;
    if (status != NULL)
        *status = 0;
    if (package == NULL || name == NULL) {
        if (name == NULL)
            p_error(p, P_ERR_TYPE, "symbol name is null");
        return 0;
    }
    if (!local_or_inherited(p, package, name, &found, &found_status))
        return 0;
    if (status != NULL)
        *status = found_status;
    return found != 0 ? found : p->nil;
}

Value p_intern(Pocket *p, Value package_value, const char *name, int *status)
{
    PPackage *package = package_ptr(p, package_value);
    Value found = 0, symbol;
    int found_status = 0;
    if (status != NULL)
        *status = 0;
    if (package == NULL || name == NULL) {
        if (name == NULL)
            p_error(p, P_ERR_TYPE, "symbol name is null");
        return 0;
    }
    if (!local_or_inherited(p, package, name, &found, &found_status))
        return 0;
    if (found != 0) {
        if (status != NULL)
            *status = found_status;
        return found;
    }
    symbol = p_make_symbol(p, name);
    if (symbol == 0)
        return 0;
    p_symbol_ptr(symbol)->home = package_value;
    if (package_value == p->keyword) {
        p_symbol_ptr(symbol)->value = symbol;
        p_symbol_ptr(symbol)->flags |= P_CONSTANT;
    }
    if (!add_entry(p, package, symbol, package_value == p->keyword))
        return 0;
    if (status != NULL)
        *status = 0;
    return symbol;
}

bool p_import(Pocket *p, Value package_value, Value symbol, bool shadowing)
{
    PPackage *package = package_ptr(p, package_value);
    PSymbol *sym;
    PPackageEntry *entry;
    if (package == NULL)
        return false;
    if (p_type(symbol) != P_SYMBOL) {
        p_error(p, P_ERR_TYPE, "expected symbol to import");
        return false;
    }
    sym = p_symbol_ptr(symbol);
    for (entry = (PPackageEntry *)package->entries; entry != NULL;
         entry = (PPackageEntry *)entry->next) {
        if (names_equal(p_symbol_ptr(entry->symbol)->name, sym->name)) {
            if (entry->symbol == symbol) {
                if (sym->home == p->nil)
                    sym->home = package_value;
                if (shadowing && !add_list_value(p, &package->shadows, symbol))
                    return false;
                return true;
            }
            if (!shadowing) {
                p_error(p, P_ERR_PACKAGE, "import conflicts with package symbol");
                return false;
            }
            if (!add_list_value(p, &package->shadows, symbol))
                return false;
            entry->symbol = symbol;
            if (sym->home == p->nil)
                sym->home = package_value;
            if (package_value == p->keyword) {
                sym->value = symbol;
                sym->flags |= P_CONSTANT;
                entry->external = true;
            }
            return true;
        }
    }
    /* Check inherited names directly because names_equal accepts stored text. */
    {
        PCons *use_cell;
        for (use_cell = (PCons *)package->uses;
             p_type((Value)use_cell) == P_CONS;
             use_cell = (PCons *)use_cell->cdr) {
            PPackage *used = (PPackage *)use_cell->car;
            for (entry = (PPackageEntry *)used->entries; entry != NULL;
                 entry = (PPackageEntry *)entry->next) {
                if (entry->external && names_equal(
                        p_symbol_ptr(entry->symbol)->name, sym->name) &&
                    entry->symbol != symbol && !shadowing) {
                    p_error(p, P_ERR_PACKAGE,
                            "import conflicts with inherited symbol");
                    return false;
                }
            }
        }
    }
    if (package_value == p->keyword) {
        sym->value = symbol;
        sym->flags |= P_CONSTANT;
    }
    if (!add_entry(p, package, symbol, package_value == p->keyword))
        return false;
    if (sym->home == p->nil)
        sym->home = package_value;
    if (shadowing)
        return add_list_value(p, &package->shadows, symbol);
    return true;
}

bool p_shadow(Pocket *p, Value package_value, const char *name)
{
    PPackage *package = package_ptr(p, package_value);
    Value symbol;
    PPackageEntry *entry;
    if (package == NULL || name == NULL) {
        if (name == NULL)
            p_error(p, P_ERR_TYPE, "symbol name is null");
        return false;
    }
    entry = entry_find_name(package, name);
    if (entry != NULL)
        return add_list_value(p, &package->shadows, entry->symbol);
    symbol = p_make_symbol(p, name);
    if (symbol == 0)
        return false;
    p_symbol_ptr(symbol)->home = package_value;
    if (!add_entry(p, package, symbol, false))
        return false;
    return add_list_value(p, &package->shadows, symbol);
}

static bool package_has_used(PPackage *package, Value used)
{
    return list_has(package->uses, used);
}

static bool name_conflicts_in_uses(Pocket *p, PPackage *package,
                                   Value symbol, Value additional)
{
    PCons *use_cell;
    PPackageEntry *entry;
    Value name = p_symbol_ptr(symbol)->name;
    PPackageEntry *local;
    if (package_shadows_name(package, name))
        return false;
    for (local = (PPackageEntry *)package->entries; local != NULL;
         local = (PPackageEntry *)local->next) {
        if (names_equal(p_symbol_ptr(local->symbol)->name, name) &&
            local->symbol != symbol) {
            p_error(p, P_ERR_PACKAGE,
                    "package has a local symbol conflicting with an export");
            return true;
        }
    }
    for (use_cell = (PCons *)package->uses;
         p_type((Value)use_cell) == P_CONS;
         use_cell = (PCons *)use_cell->cdr) {
        PPackage *used = (PPackage *)use_cell->car;
        for (entry = (PPackageEntry *)used->entries; entry != NULL;
             entry = (PPackageEntry *)entry->next) {
            if (entry->external && names_equal(
                    p_symbol_ptr(entry->symbol)->name, name) &&
                entry->symbol != symbol) {
                p_error(p, P_ERR_PACKAGE,
                        "package has conflicting inherited symbols");
                return true;
            }
        }
    }
    if (additional != 0 && !package_has_used(package, additional)) {
        PPackage *used = (PPackage *)additional;
        for (entry = (PPackageEntry *)used->entries; entry != NULL;
             entry = (PPackageEntry *)entry->next) {
            if (entry->external && names_equal(
                    p_symbol_ptr(entry->symbol)->name, name) &&
                entry->symbol != symbol) {
                p_error(p, P_ERR_PACKAGE,
                        "package has conflicting inherited symbols");
                return true;
            }
        }
    }
    return false;
}

bool p_export(Pocket *p, Value package_value, Value symbol)
{
    PPackage *package = package_ptr(p, package_value);
    PPackageEntry *entry;
    PPackage *user;
    if (package == NULL)
        return false;
    if (p_type(symbol) != P_SYMBOL) {
        p_error(p, P_ERR_TYPE, "expected symbol to export");
        return false;
    }
    entry = entry_find(package, symbol);
    if (entry == NULL) {
        if (!package_accessible_symbol(package, symbol)) {
            p_error(p, P_ERR_PACKAGE, "symbol is not present in package");
            return false;
        }
    }
    for (user = (PPackage *)p->packages; user != NULL;
         user = (PPackage *)user->next) {
        if (!package_has_used(user, package_value))
            continue;
        if (name_conflicts_in_uses(p, user, symbol, package_value))
            return false;
    }
    if (entry == NULL) {
        if (!add_entry(p, package, symbol, false))
            return false;
        entry = entry_find(package, symbol);
    }
    entry->external = true;
    return true;
}

bool p_unexport(Pocket *p, Value package_value, Value symbol)
{
    PPackage *package = package_ptr(p, package_value);
    PPackageEntry *entry;
    if (package == NULL)
        return false;
    if (p_type(symbol) != P_SYMBOL) {
        p_error(p, P_ERR_TYPE, "expected symbol to unexport");
        return false;
    }
    entry = entry_find(package, symbol);
    if (entry != NULL)
        entry->external = false;
    return true;
}

bool p_use_package(Pocket *p, Value package_value, Value used_value)
{
    PPackage *package = package_ptr(p, package_value);
    PPackage *used = package_ptr(p, used_value);
    PPackageEntry *entry;
    if (package == NULL || used == NULL)
        return false;
    if (package_value == used_value || package_has_used(package, used_value))
        return true;
    for (entry = (PPackageEntry *)used->entries; entry != NULL;
         entry = (PPackageEntry *)entry->next) {
        if (!entry->external)
            continue;
        if (name_conflicts_in_uses(p, package, entry->symbol, used_value))
            return false;
    }
    return add_list_value(p, &package->uses, used_value);
}

bool p_unuse_package(Pocket *p, Value package_value, Value used_value)
{
    PPackage *package = package_ptr(p, package_value);
    Value *link;
    if (package == NULL)
        return false;
    link = &package->uses;
    while (p_type(*link) == P_CONS) {
        PCons *cell = p_cons_ptr(*link);
        if (cell->car == used_value) {
            *link = cell->cdr;
            return true;
        }
        link = &cell->cdr;
    }
    return true;
}

static bool set_nickname(Pocket *p, Value package_value, const char *name)
{
    PPackage *package = (PPackage *)package_value;
    Value text = make_name(p, name);
    if (text == 0)
        return false;
    package->nicknames = p_cons(p, text, p->nil);
    return package->nicknames != 0;
}

static Value make_permanent_symbol(Pocket *p, const char *name, Value value,
                                   unsigned flags)
{
    Value symbol = p_make_symbol(p, name);
    if (symbol == 0)
        return 0;
    p_symbol_ptr(symbol)->value = value;
    p_symbol_ptr(symbol)->flags = flags;
    return symbol;
}

bool p_packages_init(Pocket *p)
{
    Value nil_symbol, truth_symbol, symbol, feature_list;
    int status;

    /* NIL must exist before package structures so every empty Lisp list has
     * its permanent canonical value from the beginning. */
    nil_symbol = make_permanent_symbol(p, "NIL", 0, P_CONSTANT);
    if (nil_symbol == 0)
        return false;
    p->nil = nil_symbol;
    p_symbol_ptr(nil_symbol)->value = nil_symbol;
    p_symbol_ptr(nil_symbol)->plist = p->nil;
    truth_symbol = make_permanent_symbol(p, "T", 0, P_CONSTANT);
    if (truth_symbol == 0)
        return false;
    p->truth = truth_symbol;
    p_symbol_ptr(truth_symbol)->value = truth_symbol;
    p_symbol_ptr(truth_symbol)->plist = p->nil;

    p->packages = p->package_tail = 0;
    p->cl = p_make_package(p, "COMMON-LISP");
    if (p->cl == 0 || !set_nickname(p, p->cl, "CL"))
        return false;
    p->user = p_make_package(p, "COMMON-LISP-USER");
    if (p->user == 0 || !set_nickname(p, p->user, "CL-USER"))
        return false;
    p->keyword = p_make_package(p, "KEYWORD");
    p->sys = p_make_package(p, "POCKET.SYS");
    if (p->keyword == 0 || p->sys == 0)
        return false;
    symbol = p_make_package(p, "POCKET");
    if (symbol == 0)
        return false;
    p->current_package = p->user;

    p_symbol_ptr(nil_symbol)->home = p->cl;
    p_symbol_ptr(truth_symbol)->home = p->cl;
    if (!add_entry(p, (PPackage *)p->cl, nil_symbol, true) ||
        !add_entry(p, (PPackage *)p->cl, truth_symbol, true) ||
        !p_use_package(p, p->user, p->cl))
        return false;

    symbol = p_intern(p, p->keyword, "POCKET", &status);
    if (symbol == 0)
        return false;
    p_symbol_ptr(symbol)->value = symbol;
    p_symbol_ptr(symbol)->flags |= P_CONSTANT;
    if (!p_export(p, p->keyword, symbol))
        return false;
    feature_list = p->nil;
    {
        static const char *const feature_names[] = {
            "IEEE-FLOATING-POINT", "POCKET-CL", "POCKET"
        };
        size_t i;
        for (i = 0; i < sizeof(feature_names) / sizeof(feature_names[0]); ++i) {
            symbol = p_intern(p, p->keyword, feature_names[i], &status);
            if (symbol == 0)
                return false;
            p_symbol_ptr(symbol)->value = symbol;
            p_symbol_ptr(symbol)->flags |= P_CONSTANT;
            if (!p_export(p, p->keyword, symbol))
                return false;
            feature_list = p_cons(p, symbol, feature_list);
            if (feature_list == 0)
                return false;
        }
    }
    p->features = feature_list;
    return true;
}
