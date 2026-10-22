#!/bin/sh
# Check that the canonical Common Lisp symbol manifest and its documentation agree.
# This uses only POSIX shell utilities, so SBCL is not needed for normal builds.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
manifest=$root/docs/cl-symbols.tsv
document=$root/docs/conformance.md
expected=$(mktemp "${TMPDIR:-/tmp}/pocketcl-conformance.XXXXXX")
actual=$(mktemp "${TMPDIR:-/tmp}/pocketcl-conformance.XXXXXX")
errors=$(mktemp "${TMPDIR:-/tmp}/pocketcl-conformance.XXXXXX")
trap 'rm -f "$expected" "$expected.table" "$actual" "$errors"' 0 HUP INT TERM

if [ ! -f "$manifest" ] || [ ! -f "$document" ]; then
    echo "conformance: missing docs/cl-symbols.tsv or docs/conformance.md" >&2
    exit 1
fi

# Validate the TSV and render the rows expected in the Markdown table.
if ! awk -v expected="$expected" -v errors="$errors" -F '\t' '
function fail(message) {
    print "conformance: " message > errors
    failed = 1
}
NR == 1 {
    if ($0 != "name\tkind\tstatus\tprofiles") fail("manifest header must be name, kind, status, profiles")
    next
}
{
    if (NF != 4) {
        fail("line " NR " must contain exactly four tab-separated columns")
        next
    }
    if ($1 == "" || $1 != toupper($1)) fail("line " NR " has a non-canonical symbol name: " $1)
    if (seen[$1]++) fail("duplicate symbol: " $1)
    if ($2 == "") fail("line " NR " has an empty kind")
    n = split($2, kinds, ",")
    for (i = 1; i <= n; i++) {
        if (kinds[i] !~ /^(symbol|syntax-marker|function|macro|special-operator|variable|constant|type)$/)
            fail("line " NR " has an invalid kind: " kinds[i])
    }
    if ($3 !~ /^(implemented|partial|unimplemented|excluded)$/)
        fail("line " NR " has an invalid status: " $3)
    if ($4 !~ /^(none|dev|runtime|dev,runtime)$/)
        fail("line " NR " has invalid profiles: " $4)
    if (($3 == "implemented" || $3 == "partial") && $4 == "none")
        fail("supported symbol needs an available profile: " $1)
    if (($3 == "unimplemented" || $3 == "excluded") && $4 != "none")
        fail("unsupported symbol cannot claim an available profile: " $1)
    count++
    printf "| %s | %s | %s | %s |\n", $1, $2, $3, $4 > expected
}
END {
    if (count != 978) fail("expected 978 symbol entries, found " count)
    if (failed) exit 1
}
' "$manifest"; then
    cat "$errors" >&2
    exit 1
fi

# The prose remains hand-written; the marked table must exactly reflect the manifest.
awk -v errors="$errors" '
$0 == "<!-- BEGIN GENERATED SYMBOL TABLE -->" { inside = 1; starts++; next }
$0 == "<!-- END GENERATED SYMBOL TABLE -->" { inside = 0; ends++; next }
inside { print }
END {
    if (starts != 1 || ends != 1) {
        print "conformance: generated table markers must appear exactly once" > errors
        exit 1
    }
}
' "$document" > "$actual" || { cat "$errors" >&2; exit 1; }

{ printf '| Symbol | Kind | Status | Profiles |\n| --- | --- | --- | --- |\n'; cat "$expected"; } > "$expected.table"
if ! diff -u "$expected.table" "$actual"; then
    echo "conformance: docs/conformance.md table does not match docs/cl-symbols.tsv" >&2
    rm -f "$expected.table"
    exit 1
fi
rm -f "$expected.table"
echo "conformance: 978 unique symbols; manifest and documentation agree"
