#!/usr/bin/env bash

set -e

REV_SHORT=f792b97 # f792b9756418af8ab8a91a4c15b582431cb86ff9
REV_NUM=2197 # git describe HEAD --match initial-commit | cut -d- -f3

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
