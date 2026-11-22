#!/usr/bin/env bash
# Installs the pinned MoTrack developer toolchain (tools/toolchain.env) on Ubuntu 24.04 x86-64 or
# macOS, then the project-local Qt and media SDKs.
#   Ubuntu: LLVM 23 (clang, clang-format, clang-tidy, llvm-cov/llvm-profdata, lld) from apt.llvm.org's
#           signed repository, GCC 14's libstdc++ (never libc++: Qt's Linux binaries use libstdc++),
#           and CMake, Ninja, ccache and Python 3.12 from Ubuntu.
#   macOS:  Apple Clang from the Command Line Tools stays the compiler (Qt's binaries use the system
#           libc++); Homebrew's llvm@23 supplies clang-format and clang-tidy.
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/toolchain.env
source "$repo/tools/toolchain.env"
llvm="$MOTRACK_LLVM_MAJOR"
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

printf '=== MoTrack developer setup (%s) ===\n' "$(uname -s)"
printf 'LLVM %s, GCC %s libstdc++ (Linux), Qt %s, Python %s\n' "$llvm" "$MOTRACK_GCC_MAJOR" "$MOTRACK_QT_VERSION" "$MOTRACK_PYTHON_VERSION"
if $dry_run; then printf '%s\n' '(dry-run: nothing is installed)'; fi

case "$(uname -s)" in
    Linux)
        if [[ "$(uname -m)" != x86_64 ]] || ! grep -qx 'ID=ubuntu' /etc/os-release ||
            ! grep -qx 'VERSION_ID="24.04"' /etc/os-release; then
            printf '%s\n' 'Automatic Linux setup targets Ubuntu 24.04 x86-64.' >&2
            exit 1
        fi
        sudo_cmd=()
        if [[ "$EUID" != 0 ]]; then sudo_cmd=(sudo); fi
        apt=("${sudo_cmd[@]}" apt-get -o Acquire::Retries=5)
        run "${apt[@]}" update
        run "${apt[@]}" install -y ca-certificates curl gnupg
        if $dry_run; then
            "$repo/tools/add-llvm-apt-source.sh" --dry-run
        else
            "$repo/tools/add-llvm-apt-source.sh"
        fi
        run "${apt[@]}" update
        # libclang-rt has the coverage and sanitizer runtimes (only a Recommends of clang); g++-14 brings
        # libstdc++-14-dev; libarchive-tools' bsdtar extracts Qt's 7z archives if aqtinstall is unavailable.
        # Qt Multimedia links libpulse; its FFmpeg camera backend loads libEGL, libX11, libXext and libXrandr.
        run "${apt[@]}" install -y git cmake ninja-build ccache python3 python3-venv \
            "clang-$llvm" "clang-format-$llvm" "clang-tidy-$llvm" "clangd-$llvm" "llvm-$llvm" "lld-$llvm" \
            "libclang-rt-$llvm-dev" "g++-$MOTRACK_GCC_MAJOR" libarchive-tools \
            libgl1-mesa-dev libxkbcommon-dev build-essential pkg-config autoconf automake libtool nasm patchelf \
            libxcb-cursor0 libxcb-xinerama0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
            libxcb-render-util0 libxcb-xkb1 libxkbcommon-x11-0 libfontconfig1 libfreetype6 \
            libpulse0 libegl1 libx11-6 libxext6 libxrandr2
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
        # llvm@23 is keg-only, so it never replaces Apple Clang; tools/dev-env.sh puts only its
        # clang-format and clang-tidy to use.
        run brew install cmake ninja "llvm@$llvm" ccache "python@$MOTRACK_PYTHON_VERSION" pkgconf autoconf automake libtool nasm
        if ! $dry_run; then
            PATH="$(brew --prefix "python@$MOTRACK_PYTHON_VERSION")/libexec/bin:$PATH"
            export PATH
        fi
        ;;
    *) printf '%s\n' 'Use setup-dev.ps1 on Windows.' >&2; exit 1 ;;
esac

cd "$repo"
# One SDK install for the checkout and its linked worktrees (CONTRIBUTING.md#developer-setup).
if command -v python3 >/dev/null 2>&1; then
    printf '==> Qt, FFmpeg and libarchive SDKs in %s\n' "$(python3 tools/dev_sdk.py dev-tools)"
fi
run python3 tools/dev_sdk.py install
run python3 tools/media_sdk.py install
if $dry_run; then
    printf '%s\n' '[dry-run] source tools/dev-env.sh && tools/check-prereqs.sh'
else
    # shellcheck source=tools/dev-env.sh
    source tools/dev-env.sh
    tools/check-prereqs.sh
fi
if $build; then run cmake --workflow --preset dev; fi
printf '\nFor subsequent terminals, run from the checkout:\n  source tools/dev-env.sh\n  cmake --workflow --preset dev\n'
