#!/usr/bin/env bash

set -euo pipefail


ROOT_DIR="$(
    cd "$(
        dirname "${BASH_SOURCE[0]}"
    )/.."
    pwd
)"


cd "$ROOT_DIR"


BUILD_DIR="build-release"

RESULT_DIR="results/l5/latest"


cmake \
    -S . \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release


cmake --build \
    "$BUILD_DIR" \
    -j"$(nproc)"


python3 \
    scripts/run_l5_evaluation.py \
    --build-dir "$BUILD_DIR" \
    --output "$RESULT_DIR" \
    "$@"


python3 \
    analysis/analyze_l5.py \
    "$RESULT_DIR"