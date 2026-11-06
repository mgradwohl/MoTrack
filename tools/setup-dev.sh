#!/usr/bin/env bash
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dry_run=false
build=false
for arg in "$@"; do
    case "$arg" in
        --dry-run) dry_run=true ;;
        --build) build=true ;;
        -h|--help)
            printf 'Usage: %s [--dry-run] [--build]\n' "$0"
            exit 0 ;;
        *) printf 'Unknown option: %s\n' "$arg" >&2; exit 2 ;;
    esac
done

run() {
    if $dry_run; then
        printf '[dry-run]'
        printf ' %q' "$@"
        printf '\n'
    else
        "$@"
    fi
}

case "$(uname -s)" in
    Linux)
        if [[ "$(uname -m)" != x86_64 ]] || ! grep -qx 'ID=ubuntu' /etc/os-release ||
            ! grep -qx 'VERSION_ID="24.04"' /etc/os-release; then
            printf '%s\n' 'Automatic Linux setup targets Ubuntu 24.04 x86-64.' >&2
            exit 1
        fi
        sudo_cmd=()
        if [[ "$EUID" != 0 ]]; then sudo_cmd=(sudo); fi
        run "${sudo_cmd[@]}" apt-get update
        run "${sudo_cmd[@]}" apt-get install -y git cmake ninja-build clang-18 clang-tidy-18 clangd-18 \
            llvm-18 lld-18 ccache python3 python3-venv libgl1-mesa-dev libxkbcommon-dev \
            libxcb-cursor0 libxcb-xinerama0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
            libxcb-render-util0 libxcb-xkb1 libxkbcommon-x11-0 libfontconfig1 libfreetype6
        ;;
    Darwin)
        if ! $dry_run && ! xcode-select -p >/dev/null 2>&1; then
            xcode-select --install
            printf '%s\n' 'Finish installing Apple Command Line Tools, then run setup again.' >&2
            exit 1
        fi
        if ! $dry_run && ! command -v brew >/dev/null 2>&1; then
            for brew_path in /opt/homebrew/bin /usr/local/bin; do
                if [[ -x "$brew_path/brew" ]]; then export PATH="$brew_path:$PATH"; break; fi
            done
        fi
        if ! $dry_run && ! command -v brew >/dev/null 2>&1; then
            printf '%s\n' 'Install Homebrew from https://brew.sh/ and rerun this script.' >&2
            exit 1
        fi
        run brew install cmake ninja llvm@18 ccache python@3.12
        if ! $dry_run; then
            export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
        fi
        ;;
    *) printf '%s\n' 'Use setup-dev.ps1 on Windows.' >&2; exit 1 ;;
esac

cd "$repo"
run python3 tools/dev_sdk.py install
if ! $dry_run; then
    source tools/dev-env.sh
    tools/check-prereqs.sh
fi
if $build; then run cmake --workflow --preset dev; fi
printf '\nFor subsequent terminals, run from the checkout:\n  source tools/dev-env.sh\n  cmake --workflow --preset dev\n'
