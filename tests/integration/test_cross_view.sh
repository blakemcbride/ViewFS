#!/usr/bin/env bash
# The same object is shared across views: membership is purely a function of
# the object's properties and each directory's filter, so one object surfaces
# in any view+dir whose filter it satisfies.
source "$(dirname "$0")/lib.sh"
init_store

"$VFS" view create alpha >/dev/null
"$VFS" view create beta  >/dev/null

# alpha:/by-author/blake  filters author=blake
"$VFS" dir mkdir alpha /by-author >/dev/null
"$VFS" dir mkdir alpha /by-author/blake >/dev/null
"$VFS" prop set alpha:/by-author/blake author blake >/dev/null

# beta:/2024 filters year=2024 (a different view, different filter)
"$VFS" dir mkdir beta /2024 >/dev/null
"$VFS" prop set beta:/2024 year 2024 >/dev/null

# One object with author=blake AND year=2024 satisfies both filters.
echo "shared" > "$STORE/s.txt"
sid=$("$VFS" object import "$STORE/s.txt" | awk '{print $1}')
"$VFS" prop set "$sid" author blake >/dev/null
"$VFS" prop set "$sid" year 2024 >/dev/null

assert_contains "$("$VFS" dir ls alpha /by-author/blake)" "s.txt" \
    "object visible in alpha by author"
assert_contains "$("$VFS" dir ls beta /2024)" "s.txt" \
    "same object visible in beta by year"

# An object with only author=blake is NOT in beta:/2024.
echo "only-author" > "$STORE/o.txt"
oid=$("$VFS" object import "$STORE/o.txt" | awk '{print $1}')
"$VFS" prop set "$oid" author blake >/dev/null
assert_contains     "$("$VFS" dir ls alpha /by-author/blake)" "o.txt" "o in alpha"
assert_not_contains "$("$VFS" dir ls beta /2024)" "o.txt" "o not in beta (no year)"

# Deleting the view drops its directories, not the shared object.
"$VFS" view delete beta >/dev/null
assert_contains "$("$VFS" dir ls alpha /by-author/blake)" "s.txt" \
    "shared object survives deletion of the other view"

echo "PASS: test_cross_view"
