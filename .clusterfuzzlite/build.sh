#!/bin/bash -eu

cd "$SRC/ladybird"
export RUSTC_BOOTSTRAP=1
exec Meta/Fuzzers/BuildFuzzers.sh --oss-fuzz
