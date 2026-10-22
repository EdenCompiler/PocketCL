#!/bin/sh
# Regenerate only the marked table; keep the human-written contract above it.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_table=$(mktemp "${TMPDIR:-/tmp}/pocketcl-table.XXXXXX")
trap 'rm -f "$task_table"' 0 HUP INT TERM
awk '/<!-- BEGIN GENERATED SYMBOL TABLE -->/ {exit} {print}' "$root/docs/conformance.md" > "$task_table"
printf '%s\n' '<!-- BEGIN GENERATED SYMBOL TABLE -->' '| Symbol | Kind | Status | Profiles |' '| --- | --- | --- | --- |' >> "$task_table"
awk -F '\t' 'NR>1 {printf "| %s | %s | %s | %s |\n", $1,$2,$3,$4}' "$root/docs/cl-symbols.tsv" >> "$task_table"
printf '%s\n' '<!-- END GENERATED SYMBOL TABLE -->' >> "$task_table"
cat "$task_table" > "$root/docs/conformance.md"
