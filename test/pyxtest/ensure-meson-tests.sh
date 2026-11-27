#!/bin/sh
#
# Helper script used by meson to ensure each test_foo.py file has
# a corresponding entry in the tests_pyxtest array. POSIX sh and tools:
# the BSDs have neither bash nor GNU find.

SOURCEDIR="${1:-@SOURCEDIR@}"
[ $# -gt 0 ] && shift

TESTS="${*:-@TESTS@}"
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

echo "$TESTS" | tr ' ' '\n' | grep -v '^$' | sort > "$tmp/listed"
for f in "$SOURCEDIR"/test_*.py; do
    [ -e "$f" ] && basename "$f"
done | sort > "$tmp/files"

if ! DIFF=$(diff -u "$tmp/listed" "$tmp/files"); then
    echo "ERROR: Missing test file in meson tests list:" >&2
    echo "$DIFF" >&2
    exit 1
fi
