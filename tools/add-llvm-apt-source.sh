#!/usr/bin/env bash
# Adds apt.llvm.org's llvm-toolchain-<codename>-<major> repository for the LLVM major pinned in
# tools/toolchain.env, signed by apt.llvm.org's key after checking its fingerprint. Used by
# tools/setup-dev.sh and CI. Ubuntu's own archive has no LLVM 23 packages.
#
# Usage: tools/add-llvm-apt-source.sh [--dry-run]
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/toolchain.env
source "$repo/tools/toolchain.env"
dry_run=false
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

# A source for the same repository from apt.llvm.org's llvm.sh would conflict with ours over Signed-By.
existing="$(grep -rlsE "apt\.llvm\.org/$codename/? +llvm-toolchain-$codename-$major( |$)" \
    /etc/apt/sources.list /etc/apt/sources.list.d 2>/dev/null | grep -vxF "$list" || true)"
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

# Exactly one primary key, with the pinned fingerprint: a file that appended a second primary key would
# otherwise be trusted too. Subkey fingerprints legitimately differ and are not counted.
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
