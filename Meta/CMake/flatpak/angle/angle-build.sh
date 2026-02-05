#!/usr/bin/env bash

set -e

cd angle

ninja -j"$(nproc)" -C out
