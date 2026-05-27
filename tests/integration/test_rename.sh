#!/usr/bin/env bash
# T13: Renaming a file in one view without affecting another view.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create v1 >/dev/null
"$VFS" view create v2 >/dev/null

src="$STORE/.scratch-rn"
echo 'data' > "$src"
"$VFS" object import "$src" >/dev/null
ID=$(first_object_id)
"$VFS" view add v1 "$ID" /original.txt >/dev/null
"$VFS" view add v2 "$ID" /v2-name.txt  >/dev/null

mount_view v1 "$MNT/v1"
mount_view v2 "$MNT/v2"

mv "$MNT/v1/original.txt" "$MNT/v1/renamed.txt"

l1=$(ls "$MNT/v1")
assert_contains     "$l1" 'renamed.txt'    'v1 shows new name'
assert_not_contains "$l1" 'original.txt'   'v1 no longer shows old name'

l2=$(ls "$MNT/v2")
assert_contains     "$l2" 'v2-name.txt'    'v2 still shows its mapping'
assert_not_contains "$l2" 'original.txt'   'v2 did not gain v1 name'
assert_not_contains "$l2" 'renamed.txt'    'v2 did not pick up v1 rename'

unmount_view "$MNT/v1"
unmount_view "$MNT/v2"
