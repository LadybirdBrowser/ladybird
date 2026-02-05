#!/bin/bash

files=("${@}")

while true; do
    out=$(
        for file in "${files[@]}"; do
            echo -n "$file:"
            { grep -aohE "^\s*\[[0-9]+/[0-9]+\]" "$file" || echo ' [not started yet]'; } | tail -n1
        done
    )
    clear
    echo "$out"
    sleep 1
done
