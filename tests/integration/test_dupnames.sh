#!/usr/bin/env bash
# D2: two objects with the SAME display name both match one directory.
# Through the mount they are individually addressable via a "name~idprefix"
# suffix; each resolves back to the right object.
source "$(dirname "$0")/lib.sh"
init_store

"$VFS" view create v >/dev/null
"$VFS" dir mkdir v /d >/dev/null
"$VFS" prop set v:/d k val >/dev/null

# Two distinct objects, identical name, both matching /d.
printf 'AAA\n' > "$STORE/a"; printf 'BBB\n' > "$STORE/b"
ida=$("$VFS" object import "$STORE/a" | awk '{print $1}')
idb=$("$VFS" object import "$STORE/b" | awk '{print $1}')
"$VFS" object name "$ida" report.txt >/dev/null
"$VFS" object name "$idb" report.txt >/dev/null
"$VFS" prop set "$ida" k val >/dev/null
"$VFS" prop set "$idb" k val >/dev/null

mount_view v "$MNT/v"

# readdir must show two distinct entries, both suffixed with ~<idprefix>.
mapfile -t names < <(ls -1 "$MNT/v/d" | grep -E '^report\.txt~' || true)
assert_eq "${#names[@]}" "2" "two disambiguated entries shown"
assert_contains "${names[0]}" "report.txt~" "entry carries the ~prefix suffix"

# Each suffixed name resolves to one object and cats the right bytes.
got=""
for nm in "${names[@]}"; do got="$got|$(cat "$MNT/v/d/$nm")"; done
assert_contains "$got" "AAA" "one entry yields AAA"
assert_contains "$got" "BBB" "other entry yields BBB"

# A bare, uniquely-named sibling is still shown without a suffix.
printf 'solo\n' > "$STORE/c"
cid=$("$VFS" object import "$STORE/c" | awk '{print $1}')
"$VFS" object name "$cid" unique.txt >/dev/null
"$VFS" prop set "$cid" k val >/dev/null
sleep 0.2   # let the NOTIFY-driven cache invalidation land
assert_contains "$(ls "$MNT/v/d")" "unique.txt" "uniquely-named file shown bare"
assert_eq "$(cat "$MNT/v/d/unique.txt")" "solo" "bare name cats correctly"

unmount_view "$MNT/v"
echo "PASS: test_dupnames"
