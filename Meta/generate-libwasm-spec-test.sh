#!/usr/bin/env bash

if [ $# -ne 4 ]; then
  echo "Usage: $0 <input spec file> <output path> <name> <module output path>"
  exit 1
fi

INPUT_FILE="$1"
OUTPUT_PATH="$2"
NAME="$3"
MODULE_OUTPUT_PATH="$4"

mkdir -p "$OUTPUT_PATH"
mkdir -p "$MODULE_OUTPUT_PATH"

python3 "$(dirname "$0")/generate-libwasm-spec-test.py" "$INPUT_FILE" "$NAME" "$MODULE_OUTPUT_PATH" |\
    if $SKIP_PRETTIER; then
        cat
    else
        prettier --stdin-filepath "test-$NAME.js"
    fi > "$OUTPUT_PATH/$NAME.js"
