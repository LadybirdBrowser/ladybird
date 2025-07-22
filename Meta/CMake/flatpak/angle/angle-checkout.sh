#!/usr/bin/env bash

set -e

git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git depot_tools
export PATH=$PWD/depot_tools:$PATH

gclient config https://chromium.googlesource.com/angle/angle < <(yes)
gclient sync -r chromium/7258 < <(yes)
gclient runhooks < <(yes)
