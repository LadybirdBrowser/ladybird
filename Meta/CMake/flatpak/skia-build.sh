#!/usr/bin/env bash

set -e

ninja -j"$(nproc)" -C out :skia :modules
