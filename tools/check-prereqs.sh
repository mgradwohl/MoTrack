#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo/tools/dev-env.sh"
if [[ "${1:-}" != --tools-only && "$#" != 0 ]] || (( $# > 1 )); then
    printf 'Usage: %s [--tools-only]\n' "$0" >&2
    exit 2
fi

missing=0
for tool in git cmake ninja clang++ ccache python3 clang-format clang-tidy llvm-profdata llvm-cov; do
    if path="$(command -v "$tool")" && version="$("$tool" --version 2>&1)"; then
        first_line="$(grep -Eim1 'version|^python |^[0-9]' <<< "$version" || true)"
        printf '%-14s %s [%s]\n' "$tool" "$path" "$first_line"
    else
        printf '%-14s MISSING\n' "$tool" >&2
        missing=1
    fi
done
if ((missing)); then
    printf '%s\n' 'Run tools/setup-dev.sh to install the developer toolchain.' >&2
    exit 1
fi

if [[ "${1:-}" == --tools-only ]]; then exit 0; fi
cmake --fresh -S "$repo/tools/prereqs" -B "$repo/build/prereqs" -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build "$repo/build/prereqs"
