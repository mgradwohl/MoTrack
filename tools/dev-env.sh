#!/usr/bin/env bash
# Source this file to use the MoTrack toolchain in the current shell.

# Put a directory first on PATH, once.
motrack_path_prepend() {
    local rest=":$PATH:"
    rest="${rest//:"$1":/:}"
    rest="${rest#:}"
    rest="${rest%:}"
    PATH="$1${rest:+:$rest}"
}

motrack_dev_env() {
    local repo pins llvm_major gcc_major tools qt media llvm apple gcc_dir links tool
    repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)" || return
    pins="$repo/tools/toolchain.env"
    llvm_major="$(sed -n 's/^MOTRACK_LLVM_MAJOR=//p' "$pins")"
    gcc_major="$(sed -n 's/^MOTRACK_GCC_MAJOR=//p' "$pins")"
    if command -v python3 >/dev/null 2>&1; then
        tools="$(python3 "$repo/tools/dev_sdk.py" tools-root)" || return
        qt="$(python3 "$repo/tools/dev_sdk.py" qt-prefix)" || return
        if [[ -d "$tools/venv/bin" ]]; then
            motrack_path_prepend "$tools/venv/bin"
        fi
        if [[ -f "$qt/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
            case ":${CMAKE_PREFIX_PATH:-}:" in
                *":$qt:"*) ;;
                *) export CMAKE_PREFIX_PATH="$qt${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}" ;;
            esac
            motrack_path_prepend "$qt/bin"
        fi
        media="$(python3 "$repo/tools/media_sdk.py" prefix)" || return
        if [[ -f "$media/share/ffmpeg/FindFFMPEG.cmake" ]]; then
            export MOTRACK_FFMPEG_ROOT="$media"
        fi
    fi
    # The compilers last, so they come first: ahead of the PyPI clang-format older setups put in the venv.
    case "$(uname -s)" in
        Linux)
            llvm="/usr/lib/llvm-$llvm_major/bin"
            if [[ -d "$llvm" ]]; then
                motrack_path_prepend "$llvm"
            fi
            # Clang compiles against GCC 14's libstdc++, the C++ library Qt's Linux binaries use
            # (cmake/MoTrackLibstdcxx.cmake). An explicit setting wins.
            gcc_dir="/usr/lib/gcc/$(uname -m)-linux-gnu/$gcc_major"
            if [[ -z "${MOTRACK_GCC_INSTALL_DIR:-}" && -d "$gcc_dir" && -d "/usr/include/c++/$gcc_major" ]]; then
                export MOTRACK_GCC_INSTALL_DIR="$gcc_dir"
            fi
            ;;
        Darwin)
            if [[ -z "${SDKROOT:-}" ]]; then
                SDKROOT="$(xcrun --show-sdk-path)" || return
                export SDKROOT
            fi
            # Apple's toolchain: Apple Clang (Qt's macOS binaries use the system libc++) and the llvm-cov
            # and llvm-profdata that read its coverage profiles.
            apple="$(xcrun --find clang++)" || return
            motrack_path_prepend "${apple%/*}"
            # Xcode's toolchain has its own, older clang-format, so links to just clang-format and
            # clang-tidy from Homebrew's keg-only llvm@23 go ahead of it; the rest of llvm@23, its
            # clang++ included, stays off PATH.
            if command -v brew >/dev/null 2>&1; then
                llvm="$(brew --prefix "llvm@$llvm_major" 2>/dev/null)/bin"
                links="$repo/.dev-tools/macos/llvm-$llvm_major-tools"
                if [[ -x "$llvm/clang-format" ]] && mkdir -p "$links"; then
                    for tool in clang-format clang-tidy clang-apply-replacements run-clang-tidy; do
                        if [[ -x "$llvm/$tool" ]]; then ln -sf "$llvm/$tool" "$links/$tool"; fi
                    done
                    motrack_path_prepend "$links"
                fi
            fi
            ;;
    esac
    export PATH
}

motrack_dev_env
