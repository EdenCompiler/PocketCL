#!/bin/sh
# Validate actual exports as well as the offline inventory. Role checks are
# deliberate: lambda-list markers need no function binding, while macros do.
set -eu
binary=${1:-build/default/pocketvm0}
profile=${2:-dev}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_exports=$(mktemp "${TMPDIR:-/tmp}/pocketcl-exports.XXXXXX")
trap 'rm -f "$task_exports"' 0 HUP INT TERM
"$binary" --exports > "$task_exports"
awk -v profile="$profile" -F '\t' '
NR==FNR { if (FNR>1) { kind[$1]=$2; status[$1]=$3; profiles[$1]=$4 }; next }
{
  if (!($1 in kind)) { print "nonstandard COMMON-LISP export: " $1; failed=1 }
  else if (status[$1] == "unimplemented" || status[$1] == "excluded") {
    print "exported symbol has unsupported manifest status: " $1; failed=1
  }
  else if (index("," kind[$1] ",", "," $2 ",") == 0) {
    print "runtime role disagrees with manifest: " $1 " (" $2 ")"; failed=1
  }
  else if (index("," profiles[$1] ",", "," profile ",") == 0) {
    print "export missing requested profile: " $1 " (" profile ")"; failed=1
  }
  exported[$1]=1; count++
}
END {
  for (name in status)
    if (index("," profiles[name] ",", "," profile ",") != 0 && !(name in exported)) {
      print "supported manifest symbol is not exported: " name; failed=1
    }
  if (failed) exit 1
  print "runtime conformance: " count " standard exports match the manifest"
}
' "$root/docs/cl-symbols.tsv" "$task_exports"
