#!/bin/sh
# Compare retained VM heap usage for a short and long proper-tail call in one
# process. Both samples run the same compiled function and collect at the same
# explicit --stats safe point.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
binary=${1:-build/default/pocketvm}
if [ ! -x "$binary" ]; then
    echo "VM memory check: executable not found: $binary" >&2
    exit 1
fi

definition='(defun m3-memory-tail (remaining total) (if (= remaining 0) total (m3-memory-tail (1- remaining) (1+ total))))'
output=$("$binary" \
    -e "$definition" \
    -e '(m3-memory-tail 1000 17)' --stats \
    -e '(m3-memory-tail 1000000 17)' --stats)
printf '%s\n' "$output"

printf '%s\n' "$output" | awk '
function field_value(key, count, fields, i, prefix) {
    prefix = key "="
    for (i = 1; i <= count; ++i)
        if (index(fields[i], prefix) == 1)
            return substr(fields[i], length(prefix) + 1)
    return ""
}
/^[0-9]+$/ {
    ++result_count
    if (result_count == 1) first_result = $0
    else if (result_count == 2) second_result = $0
}
/^arena=/ {
    count = split($0, fields, " ")
    arena = field_value("arena", count, fields)
    live = field_value("live", count, fields)
    if (arena == "" || live == "") {
        print "VM memory check: malformed --stats output" > "/dev/stderr"
        exit 1
    }
    ++stats_count
    if (stats_count == 1) { first_arena = arena; first_live = live }
    else if (stats_count == 2) { second_arena = arena; second_live = live }
}
END {
    if (result_count != 2 || first_result != 1017 || second_result != 1000017) {
        print "VM memory check: unexpected tail-call results" > "/dev/stderr"
        exit 1
    }
    if (stats_count != 2) {
        print "VM memory check: expected two --stats records" > "/dev/stderr"
        exit 1
    }
    if (first_arena != second_arena) {
        print "VM memory check: arena size changed between samples" > "/dev/stderr"
        exit 1
    }
    if (first_live != second_live) {
        print "VM memory check: retained live bytes grew from " first_live " to " second_live > "/dev/stderr"
        exit 1
    }
    print "VM memory check: 1,000 and 1,000,000 tail calls retain " first_live " bytes"
}
'
