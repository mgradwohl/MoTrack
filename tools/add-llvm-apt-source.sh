#!/usr/bin/env bash
# Adds apt.llvm.org's llvm-toolchain-<codename>-<major> repository for the LLVM major pinned in
# tools/toolchain.env, signed by apt.llvm.org's key after checking its fingerprint. Used by
# tools/setup-dev.sh and CI. Ubuntu's own archive has no LLVM 23 packages.
#
# Usage: tools/add-llvm-apt-source.sh [--dry-run]
set -euo pipefail

# The APT source files under $1 (/etc/apt) other than $4 that already enable apt.llvm.org's
# llvm-toolchain-$2-$3 suite, one per line: one-line entries in sources.list and *.list, and deb822
# stanzas in *.sources (apt.llvm.org's llvm.sh writes either). A second entry for the same repository
# with another Signed-By makes apt-get update fail.
existing_llvm_sources() {
    local apt="$1" codename="$2" major="$3" own="$4" file suite="llvm-toolchain-$2-$3"
    for file in "$apt/sources.list" "$apt"/sources.list.d/*.list; do
        if [[ -f "$file" && "$file" != "$own" ]] &&
            grep -qE "^[[:space:]]*deb(-src)?[[:space:]].*apt\.llvm\.org/$codename/?[[:space:]]+$suite([[:space:]]|$)" "$file"; then
            printf '%s\n' "$file"
        fi
    done
    for file in "$apt"/sources.list.d/*.sources; do
        if [[ -f "$file" ]] && awk -v suite="$suite" '
            BEGIN { RS = "" }
            {
                uris = 0; listed = 0; enabled = 1
                lines = split($0, line, "\n")
                for (i = 1; i <= lines; i++) {
                    if (line[i] ~ /^[ \t]*#/) continue
                    if (line[i] ~ /^URIs:/ && line[i] ~ /apt\.llvm\.org/) uris = 1
                    if (line[i] ~ /^Suites:/) {
                        words = " " substr(line[i], 8) " "
                        gsub(/[ \t]+/, " ", words)
                        if (index(words, " " suite " ")) listed = 1
                    }
                    if (line[i] ~ /^Enabled:[ \t]*[Nn][Oo]/) enabled = 0
                }
                if (uris && listed && enabled) found = 1
            }
            END { exit !found }' "$file"; then
            printf '%s\n' "$file"
        fi
    done
}

main() {
    # key stays global: the EXIT trap removes it after main returns.
    local repo dry_run=false codename major keyring list line existing fingerprint record want primary sudo_cmd
    repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
    # shellcheck source=tools/toolchain.env
    source "$repo/tools/toolchain.env"
    case "${1:-}" in
        '') ;;
        --dry-run) dry_run=true ;;
        *) printf 'Usage: %s [--dry-run]\n' "$0" >&2; exit 2 ;;
    esac

    # shellcheck disable=SC1091
    codename="$(. /etc/os-release && echo "${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}")"
    if [[ -z "$codename" ]]; then
        printf '%s\n' 'Could not read the Ubuntu codename from /etc/os-release.' >&2
        exit 1
    fi
    major="$MOTRACK_LLVM_MAJOR"
    keyring=/usr/share/keyrings/motrack-apt-llvm-org.gpg
    list="/etc/apt/sources.list.d/motrack-llvm-$major.list"
    line="deb [signed-by=$keyring] https://apt.llvm.org/$codename/ llvm-toolchain-$codename-$major main"

    if $dry_run; then
        printf '[dry-run] verify https://apt.llvm.org/llvm-snapshot.gpg.key is %s, install it as %s\n' \
            "$MOTRACK_LLVM_APT_KEY_FINGERPRINT" "$keyring"
        printf '[dry-run] %s > %s\n' "$line" "$list"
        exit 0
    fi

    existing="$(existing_llvm_sources /etc/apt "$codename" "$major" "$list")"
    if [[ -n "$existing" ]]; then
        printf 'apt.llvm.org llvm-toolchain-%s-%s is already configured (%s); leaving it.\n' "$codename" "$major" "${existing//$'\n'/, }"
        exit 0
    fi

    sudo_cmd=()
    if [[ "$EUID" != 0 ]]; then sudo_cmd=(sudo); fi
    key="$(mktemp)"
    trap 'rm -f "$key"' EXIT
    curl --fail --silent --show-error --location --connect-timeout 20 --max-time 120 \
        --retry 5 --retry-delay 10 --retry-all-errors --output "$key" https://apt.llvm.org/llvm-snapshot.gpg.key

    # Exactly one primary key, with the pinned fingerprint: a file that appended a second primary key
    # would otherwise be trusted too. Subkey fingerprints legitimately differ and are not counted.
    primary=()
    want=0
    while IFS=: read -r record _ _ _ _ _ _ _ _ fingerprint _; do
        case "$record" in
            pub) want=1 ;;
            fpr) if ((want)); then primary+=("$fingerprint"); fi; want=0 ;;
            *) want=0 ;;
        esac
    done < <(gpg --show-keys --with-fingerprint --with-colons "$key" 2>/dev/null)
    if [[ "${#primary[@]}" != 1 || "${primary[0]}" != "$MOTRACK_LLVM_APT_KEY_FINGERPRINT" ]]; then
        printf 'apt.llvm.org key fingerprint(s) %s, expected exactly %s. Not trusting it.\n' \
            "${primary[*]:-none}" "$MOTRACK_LLVM_APT_KEY_FINGERPRINT" >&2
        printf '%s\n' 'If apt.llvm.org rotated its key, confirm the new one at https://apt.llvm.org/ before updating tools/toolchain.env.' >&2
        exit 1
    fi
    gpg --dearmor < "$key" | "${sudo_cmd[@]}" tee "$keyring" >/dev/null
    printf '%s\n' "$line" | "${sudo_cmd[@]}" tee "$list" >/dev/null
    printf 'Added %s (key %s).\n' "$list" "$MOTRACK_LLVM_APT_KEY_FINGERPRINT"
}

# Sourced by tests/test_dev_tools.py to test existing_llvm_sources.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
