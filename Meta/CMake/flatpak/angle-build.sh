#!/usr/bin/env bash

set -e

export PATH=$PWD/depot_tools:$PATH
cd angle

ninja -j"$(nproc)" -C out
