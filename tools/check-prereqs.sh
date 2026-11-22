#!/usr/bin/env bash
# Checks the MoTrack developer tools against tools/toolchain.env, then builds a small Qt, FFmpeg and
# libarchive probe. Each failure prints how to fix it; exits 1 when anything required is missing.
#   Linux: clang, clang++, clang-format, clang-tidy, llvm-cov and llvm-profdata must be LLVM 23, and
#          clang++ must compile against libstdc++ from GCC 14 or newer (never libc++).
#   macOS: clang and clang++ must be Apple Clang 17+ with its llvm-cov and llvm-profdata;
#          clang-format and clang-tidy must be LLVM 23 (Homebrew's llvm@23).
#   Both:  CMake, Ninja, Python, Git and Qt must meet their floors, and Qt must include Qt Multimedia
#          (on Linux with the PulseAudio client library it links); ccache is recommended.
#
# Usage: tools/check-prereqs.sh [--tools-only]   (--tools-only skips the probe; a missing Qt or Qt Multimedia only warns)
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/toolchain.env
source "$repo/tools/toolchain.env"
setup_hint='Run tools/setup-dev.sh (preview with --dry-run), then source tools/dev-env.sh.'

# Fields are separated by the ASCII unit separator: read would merge adjacent tabs, dropping an empty field.
sep=$'\x1f'
rows=()
add_row() { # status name version required detail fix
    rows+=("$1$sep$2$sep$3$sep$4$sep$5$sep$6")
}

# 0 when dotted version $1 >= $2 (in Bash: macOS's sort has no -V).
version_ge() {
    local IFS=. i x y
    local -a have=($1) want=($2)
    for ((i = 0; i < ${#have[@]} || i < ${#want[@]}; i++)); do
        x="${have[i]:-0}" y="${want[i]:-0}"
        if ((10#$x != 10#$y)); then
            ((10#$x > 10#$y))
            return
        fi
    done
}

# The first X.Y[.Z] after $2 in the output of "$1 --version" (or of the remaining arguments).
tool_version() {
    local tool="$1" prefix="$2"
    shift 2
    if (($# == 0)); then set -- --version; fi
    { "$tool" "$@" 2>&1 || true; } | grep -oE "${prefix}[0-9]+\.[0-9]+(\.[0-9]+)?" | head -n 1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?$' || true
}

# A row for an LLVM tool that must be the pinned major.
check_llvm_tool() {
    local name="$1" path release
    path="$(command -v "$name" || true)"
    if [[ -z "$path" ]]; then
        add_row fail "$name" '' "= $MOTRACK_LLVM_MAJOR" '(not on PATH)' "$name comes with LLVM $MOTRACK_LLVM_MAJOR. $setup_hint"
        return
    fi
    release="$(tool_version "$path" '(clang|clang-format|LLVM) version ')"
    if [[ -z "$release" ]]; then
        add_row fail "$name" unknown "= $MOTRACK_LLVM_MAJOR" "$path" "$path --version printed no LLVM version. $setup_hint"
    elif [[ "${release%%.*}" != "$MOTRACK_LLVM_MAJOR" ]]; then
        local fix="$path is LLVM $release; MoTrack formats, analyzes and builds with LLVM $MOTRACK_LLVM_MAJOR only."
        case "$path" in
            */venv/bin/*) fix+=' It is the PyPI clang-format older setups installed: python3 tools/dev_sdk.py install removes it.' ;;
            *) fix+=" $setup_hint" ;;
        esac
        add_row fail "$name" "$release" "= $MOTRACK_LLVM_MAJOR" "$path" "$fix"
    else
        add_row ok "$name" "$release" "= $MOTRACK_LLVM_MAJOR" "$path" ''
    fi
}

# macOS: a row for Apple Clang or one of its LLVM tools, which must come from Apple's toolchain.
check_apple_tool() {
    local name="$1" minimum="$2" path line version
    path="$(command -v "$name" || true)"
    if [[ -z "$path" ]]; then
        add_row fail "$name" '' "Apple >= $minimum" '(not on PATH)' 'Install the Command Line Tools (xcode-select --install), then source tools/dev-env.sh.'
        return
    fi
    line="$({ "$path" --version 2>&1 || true; } | grep -m 1 -E 'version [0-9]' || true)"
    version="$(grep -oE 'version [0-9]+(\.[0-9]+)*' <<< "$line" | head -n 1 | cut -d' ' -f2 || true)"
    if [[ "$line" != *Apple* ]]; then
        add_row fail "$name" "${version:-unknown}" "Apple >= $minimum" "$path" "macOS builds use Apple Clang and its tools, as Qt's binaries use the system libc++; source tools/dev-env.sh puts $(dirname "$(xcrun --find clang++ 2>/dev/null || echo /usr/bin/clang++)") first."
    elif ! version_ge "${version%%.*}" "$minimum"; then
        add_row fail "$name" "$version" "Apple >= $minimum" "$path" "Install Xcode or Command Line Tools 16.3 or newer (macos-15 CI uses Xcode 16.4)."
    else
        add_row ok "$name" "$version" "Apple >= $minimum" "$path" ''
    fi
}

# Linux: clang++ must compile against libstdc++ from GCC $MOTRACK_GCC_MAJOR or newer.
check_libstdcxx() {
    local args=() release selected dir="${MOTRACK_GCC_INSTALL_DIR:-}"
    local install="sudo apt-get install g++-$MOTRACK_GCC_MAJOR, then source tools/dev-env.sh (it sets MOTRACK_GCC_INSTALL_DIR)"
    if ! command -v clang++ >/dev/null 2>&1; then
        add_row fail libstdc++ '' ">= $MOTRACK_GCC_MAJOR" '(no clang++)' "$setup_hint"
        return
    fi
    if [[ -n "$dir" ]]; then args=("--gcc-install-dir=$dir"); fi
    selected="$({ clang++ ${args[@]+"${args[@]}"} -v -x c++ -E -o /dev/null /dev/null 2>&1 || true; } | sed -n 's/^Selected GCC installation: //p' | head -n 1)"
    # <version> brings declarations along, so the answer is the line after a marker.
    release="$(printf '%s\n' '#include <version>' '#ifdef _LIBCPP_VERSION' 'MOTRACK_STDLIB=libc++' '#elif defined(_GLIBCXX_RELEASE)' \
        'MOTRACK_STDLIB=_GLIBCXX_RELEASE' '#endif' |
        { clang++ ${args[@]+"${args[@]}"} -std=c++23 -x c++ -E -P - 2>/dev/null || true; } | sed -n 's/^[[:space:]]*MOTRACK_STDLIB=[[:space:]]*\([^[:space:]]*\).*$/\1/p' | head -n 1)"
    if [[ "$release" == libc++ ]]; then
        add_row fail libstdc++ libc++ ">= $MOTRACK_GCC_MAJOR" "${selected:-$dir}" "Linux builds use libstdc++ (Qt's binaries do); remove -stdlib=libc++ from CXXFLAGS or the clang++ configuration."
    elif [[ ! "$release" =~ ^[0-9]+$ ]]; then
        add_row fail libstdc++ '' ">= $MOTRACK_GCC_MAJOR" "${selected:-${dir:-(no GCC installation)}}" "clang++ found no libstdc++ headers. $install."
    elif ((release < MOTRACK_GCC_MAJOR)); then
        add_row fail libstdc++ "$release" ">= $MOTRACK_GCC_MAJOR" "${selected:-$dir}" "GCC $release's libstdc++ lacks C++23 parts MoTrack may use. $install."
    elif [[ -z "$dir" ]]; then
        add_row warn libstdc++ "$release" ">= $MOTRACK_GCC_MAJOR" "$selected" "Clang chose this GCC by itself; source tools/dev-env.sh to select GCC $MOTRACK_GCC_MAJOR explicitly, as CI does."
    else
        add_row ok libstdc++ "$release" ">= $MOTRACK_GCC_MAJOR" "${selected:-$dir}" ''
    fi
}

# A row for a tool with a floor; "optional" as $6 makes a miss a warning.
check_minimum() {
    local name="$1" path="$2" version="$3" minimum="$4" fix="$5" bad=fail
    if [[ "${6:-}" == optional ]]; then bad=warn; fi
    if [[ -z "$version" ]]; then
        add_row "$bad" "$name" '' ">= $minimum" "${path:-(not found)}" "$fix"
    elif ! version_ge "$version" "$minimum"; then
        add_row "$bad" "$name" "$version" ">= $minimum" "$path" "$fix"
    else
        add_row ok "$name" "$version" ">= $minimum" "$path" ''
    fi
}

# A row for Qt Multimedia (camera capture) in Qt prefix $1 (version $2); "optional" as $3 makes a miss a warning.
check_qt_multimedia() {
    local prefix="$1" version="$2" bad=fail required="Qt $MOTRACK_QT_MIN+ module"
    local fix="python3 tools/dev_sdk.py install adds Qt Multimedia to the .dev-tools Qt; another Qt prefix needs its qtmultimedia module (aqt install-qt ... -m qtmultimedia)."
    if [[ "${3:-}" == optional ]]; then bad=warn; fi
    if [[ -z "$prefix" ]]; then
        add_row "$bad" "Qt Multimedia" '' "$required" '(no Qt)' "$fix"
    elif [[ -f "$prefix/lib/cmake/Qt6Multimedia/Qt6MultimediaConfig.cmake" ]]; then
        add_row ok "Qt Multimedia" "$version" "$required" "$prefix/lib/cmake/Qt6Multimedia" ''
    else
        add_row "$bad" "Qt Multimedia" '' "$required" "(not in $prefix)" "$fix"
    fi
}

# Linux: Qt Multimedia links the PulseAudio client library, so building and starting MoTrack need it.
check_libpulse() {
    local dir
    for dir in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do
        if [[ -e "$dir/libpulse.so.0" ]]; then
            add_row ok libpulse so.0 installed "$dir/libpulse.so.0" ''
            return
        fi
    done
    add_row fail libpulse '' installed '(not found)' 'Qt Multimedia needs the PulseAudio client library: sudo apt-get install libpulse0.'
}

# The Qt prefix CMake finds first and its version, tab-separated, or nothing.
find_qt() {
    local candidate qtpaths prefixes candidates=()
    if [[ -n "${Qt6_DIR:-}" ]]; then candidates+=("$Qt6_DIR" "$Qt6_DIR/../../.."); fi
    if [[ -n "${QT_ROOT_DIR:-}" ]]; then candidates+=("$QT_ROOT_DIR"); fi
    if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
        IFS=: read -r -a prefixes <<< "$CMAKE_PREFIX_PATH"
        candidates+=("${prefixes[@]}")
    fi
    if qtpaths="$(command -v qtpaths || command -v qtpaths6)"; then candidates+=("$(dirname "$(dirname "$qtpaths")")"); fi
    # Bash 3.2 (macOS) treats expanding an empty array as unset under set -u.
    if ((${#candidates[@]} == 0)); then return; fi
    for candidate in "${candidates[@]}"; do
        if [[ -n "$candidate" && -f "$candidate/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake" ]]; then
            printf '%s\t%s\n' "$(cd "$candidate" && pwd)" \
                "$(sed -n 's/^set(PACKAGE_VERSION "\([0-9.]*\)")$/\1/p' "$candidate/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake" | head -n 1)"
            return
        fi
    done
}

print_rows() {
    local row status name version required detail fix mark color reset='' fixes=()
    if [[ -t 1 ]]; then reset=$'\033[0m'; fi
    printf '%-16s%-14s%-18s%s\n' Tool Version Required Path
    printf '%s\n' "$(printf -- '-%.0s' {1..86})"
    for row in "${rows[@]}"; do
        IFS="$sep" read -r status name version required detail fix <<< "$row"
        case "$status" in
            ok) mark='✓' color=$'\033[0;32m' ;;
            warn) mark='!' color=$'\033[1;33m' ;;
            *) mark='✗' color=$'\033[0;31m' ;;
        esac
        if [[ ! -t 1 ]]; then color=''; fi
        printf '%s%s%s %-14s %s%-13s%s %-17s %s\n' "$color" "$mark" "$reset" "$name" "$color" "${version:-NOT FOUND}" "$reset" "($required)" "$detail"
        if [[ "$status" != ok && -n "$fix" ]]; then fixes+=("$name: $fix"); fi
    done
    printf '%s\n' "$(printf -- '-%.0s' {1..86})"
    if ((${#fixes[@]})); then printf '%s\n' "${fixes[@]}"; fi
}

main() {
    local tools_only=false
    case "${1:-}" in
        '') ;;
        --tools-only) tools_only=true ;;
        *) printf 'Usage: %s [--tools-only]\n' "$0" >&2; exit 2 ;;
    esac
    # shellcheck source=tools/dev-env.sh
    source "$repo/tools/dev-env.sh"

    local name path qt
    case "$(uname -s)" in
        Linux)
            for name in clang clang++ clang-format clang-tidy llvm-cov llvm-profdata; do check_llvm_tool "$name"; done
            check_libstdcxx
            ;;
        Darwin)
            check_apple_tool clang "$MOTRACK_APPLE_CLANG_MIN"
            check_apple_tool clang++ "$MOTRACK_APPLE_CLANG_MIN"
            for name in clang-format clang-tidy; do check_llvm_tool "$name"; done
            for name in llvm-cov llvm-profdata; do check_apple_tool "$name" "$MOTRACK_APPLE_CLANG_MIN"; done
            ;;
        *) printf '%s\n' 'Use check-prereqs.ps1 on Windows.' >&2; exit 1 ;;
    esac
    path="$(command -v cmake || true)"
    check_minimum cmake "$path" "$(tool_version "${path:-cmake}" 'cmake version ')" "$MOTRACK_CMAKE_MIN" "Install CMake $MOTRACK_CMAKE_MIN or newer. $setup_hint"
    path="$(command -v ninja || true)"
    check_minimum ninja "$path" "$(tool_version "${path:-ninja}" '')" "$MOTRACK_NINJA_MIN" "Install Ninja $MOTRACK_NINJA_MIN or newer. $setup_hint"
    path="$(command -v ccache || true)"
    check_minimum ccache "$path" "$(tool_version "${path:-ccache}" 'ccache version ')" "$MOTRACK_CCACHE_MIN" 'Recommended for faster rebuilds; tools/setup-dev.sh installs it.' optional
    path="$(command -v python3 || true)"
    check_minimum python3 "$path" "$(tool_version "${path:-python3}" 'Python ')" "$MOTRACK_PYTHON_VERSION" "Install Python $MOTRACK_PYTHON_VERSION or newer. $setup_hint"
    path="$(command -v git || true)"
    check_minimum git "$path" "$(tool_version "${path:-git}" 'git version ')" "$MOTRACK_GIT_MIN" "Install Git $MOTRACK_GIT_MIN or newer."
    qt="$(find_qt)"
    check_minimum Qt "${qt%%$'\t'*}" "${qt#*$'\t'}" "$MOTRACK_QT_MIN" \
        "python3 tools/dev_sdk.py install puts Qt $MOTRACK_QT_VERSION in .dev-tools; or add a Qt $MOTRACK_QT_MIN+ prefix to CMAKE_PREFIX_PATH." \
        "$($tools_only && echo optional || true)"
    check_qt_multimedia "${qt%%$'\t'*}" "${qt#*$'\t'}" "$($tools_only && echo optional || true)"
    if [[ "$(uname -s)" == Linux ]]; then check_libpulse; fi
    if [[ "$(uname -s)" == Linux ]] && ! command -v bsdtar >/dev/null 2>&1; then
        add_row warn bsdtar '' '' '(not found)' "Only needed when aqtinstall is unreachable and Qt comes from download.qt.io: sudo apt-get install libarchive-tools."
    fi

    print_rows
    local row
    for row in "${rows[@]}"; do
        if [[ "$row" == "fail$sep"* ]]; then
            printf '%s\n' 'Some prerequisites are missing or outside the versions MoTrack needs.' >&2
            exit 1
        fi
    done
    if $tools_only; then
        printf '%s\n' 'The required tools are installed.'
        exit 0
    fi
    cmake --fresh -S "$repo/tools/prereqs" -B "$repo/build/prereqs" -G Ninja -DCMAKE_CXX_COMPILER=clang++ || {
        printf '%s\n' 'Could not configure the probe. python3 tools/dev_sdk.py install and python3 tools/media_sdk.py install provide Qt, FFmpeg and libarchive.' >&2
        exit 1
    }
    cmake --build "$repo/build/prereqs" || {
        printf '%s\n' 'Could not build the probe; see the compiler output above.' >&2
        exit 1
    }
    printf '%s\n' 'The required tools are installed and the probe builds.'
}

# Sourced by tests/test_dev_tools.py to test the functions; the checks run only as a script.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
