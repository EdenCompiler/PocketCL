#!/bin/sh
# Reference-machine gate. Each timed sample starts a fresh process, including
# image/boot load, and must return the same Fibonacci value before comparison.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
stage0=${1:-build/default/pocketvm0}
vm=${2:-build/default/pocketvm}
output=${3:-build/default/benchmark}
mkdir -p "$output"
definition='(defun fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))'
printf 'engine\tsample\treal_seconds\n' > "$output/samples.tsv"
for engine in stage0 vm; do
    if [ "$engine" = stage0 ]; then binary=$stage0; else binary=$vm; fi
    sample=1
    while [ "$sample" -le 3 ]; do
        /usr/bin/time -p -o "$output/$engine-$sample.time" \
            "$binary" -e "$definition" -e '(fib 30)' > "$output/$engine-$sample.result"
        if [ "$(tail -n 1 "$output/$engine-$sample.result")" != 832040 ]; then
            echo "benchmark: wrong $engine Fibonacci result" >&2
            exit 1
        fi
        elapsed=$(awk '$1=="real" {print $2}' "$output/$engine-$sample.time")
        printf '%s\t%s\t%s\n' "$engine" "$sample" "$elapsed" >> "$output/samples.tsv"
        sample=$((sample + 1))
    done
done
for engine in stage0 vm; do
    awk -F '\t' -v engine="$engine" '$1==engine {print $3}' "$output/samples.tsv" |
        sort -n > "$output/$engine.sorted"
done
stage0_median=$(sed -n '2p' "$output/stage0.sorted")
vm_median=$(sed -n '2p' "$output/vm.sorted")
awk -v stage0="$stage0_median" -v vm="$vm_median" 'BEGIN {
    if (vm <= 0) { print "benchmark: timer resolution too low"; exit 1 }
    printf "Fibonacci 30: stage0 %.2fs, VM %.2fs, speedup %.2fx\n", stage0, vm, stage0/vm
    if (stage0/vm < 5) exit 1
}'
