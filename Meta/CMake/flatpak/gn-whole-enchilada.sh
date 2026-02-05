#!/usr/bin/env bash

set -e

REV_SHORT=97b68a0 # 97b68a0bb62b7528bc3491c7949d6804223c2b82
REV_NUM=2255 # git describe HEAD --match initial-commit | cut -d- -f3

env CC=gcc CXX=g++ python3 build/gen.py --no-last-commit-position

cat > out/last_commit_position.h <<EOF
    #ifndef OUT_LAST_COMMIT_POSITION_H_
    #define OUT_LAST_COMMIT_POSITION_H_

    #define LAST_COMMIT_POSITION_NUM ${REV_NUM}
    #define LAST_COMMIT_POSITION "${REV_NUM} (${REV_SHORT})"

    #endif  // OUT_LAST_COMMIT_POSITION_H_
EOF

ninja -j"$(nproc)" -C out

cp -v out/gn "$FLATPAK_DEST"/bin/
