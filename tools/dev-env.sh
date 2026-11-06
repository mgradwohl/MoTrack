#!/usr/bin/env bash
# Source this file to use the MoTrack toolchain in the current shell.

motrack_dev_env() {
    local repo llvm tools qt
    repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)" || return
    case "$(uname -s)" in
        Linux) llvm=/usr/lib/llvm-18/bin ;;
        Darwin)
            if command -v brew >/dev/null 2>&1; then
                llvm="$(brew --prefix llvm@18)/bin"
            fi
            if [[ -z "${SDKROOT:-}" ]]; then
                SDKROOT="$(xcrun --show-sdk-path)" || return
                export SDKROOT
            fi
            ;;
    esac
    if [[ -n "${llvm:-}" && -d "$llvm" ]]; then
        export PATH="$llvm:$PATH"
    fi
    if command -v python3 >/dev/null 2>&1; then
        tools="$(python3 "$repo/tools/dev_sdk.py" tools-root)" || return
        qt="$(python3 "$repo/tools/dev_sdk.py" qt-prefix)" || return
        if [[ -d "$tools/venv/bin" ]]; then
            export PATH="$tools/venv/bin:$PATH"
        fi
        if [[ -f "$qt/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
            case ":${CMAKE_PREFIX_PATH:-}:" in
                *":$qt:"*) ;;
                *) export CMAKE_PREFIX_PATH="$qt${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}" ;;
            esac
            export PATH="$qt/bin:$PATH"
        fi
    fi
}

motrack_dev_env
