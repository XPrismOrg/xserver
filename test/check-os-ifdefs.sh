#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# OS-specific code belongs in the Platform Abstraction Layer (os/pal, see
# doc/pal.md) or the legacy per-OS directories. This fails if any other
# file gains an OS #if/#ifdef: a file not in the baseline, or a file with
# more of them than the baseline allows.
#
#   check-os-ifdefs.sh SRCDIR            check
#   check-os-ifdefs.sh SRCDIR --update   rewrite the baseline (only to
#                                        shrink it, after moving code
#                                        into os/pal)

set -euo pipefail

srcdir=${1:?usage: $0 SRCDIR [--update]}
baseline="$srcdir/test/os-ifdefs.baseline"

os_macros='__linux__|__FreeBSD__|__FreeBSD_kernel__|__OpenBSD__|__NetBSD__|__DragonFly__|__sun|__SVR4|__GNU__|__APPLE__|__CYGWIN__|CSRG_BASED'
pattern="^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif).*\\b(${os_macros})\\b"

current() {
    cd "$srcdir"
    grep -rEc --include='*.c' --include='*.h' "$pattern" . 2>/dev/null |
        grep -v ':0$' |
        sed 's|^\./||' |
        grep -vE '^(os/pal/|hw/xfree86/os-support/|hw/kdrive/os/|test/|build)' |
        LC_ALL=C sort || true
}

if [[ ${2:-} == --update ]]; then
    current > "$baseline"
    echo "baseline: $(wc -l < "$baseline") files"
    exit 0
fi

fail=0
while IFS=: read -r file count; do
    allowed=$(grep -F "$file:" "$baseline" | head -n1 | cut -d: -f2 || true)
    if [[ -z $allowed ]]; then
        echo "NEW: $file has $count OS #if(s); move the code into os/pal (doc/pal.md)"
        fail=1
    elif (( count > allowed )); then
        echo "MORE: $file has $count OS #if(s), baseline allows $allowed"
        fail=1
    elif (( count < allowed )); then
        echo "note: $file has fewer OS #if(s) ($count) than the baseline ($allowed); run with --update"
    fi
done < <(current)

exit $fail
