#!/usr/bin/env bash
# T14: Removing a file from one view without deleting the object
#      from another view.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create v1 >/dev/null
"$VFS" view create v2 >/dev/null

src="$STORE/.scratch-u"
echo 'shared content' > "$src"
"$VFS" object import "$src" >/dev/null
ID=$(first_object_id)
"$VFS" view add v1 "$ID" /file.txt >/dev/null
"$VFS" view add v2 "$ID" /file.txt >/dev/null

mount_view v1 "$MNT/v1"
mount_view v2 "$MNT/v2"

rm "$MNT/v1/file.txt"

l1=$(ls "$MNT/v1")
assert_not_contains "$l1" 'file.txt' 'v1 lost the file'

l2=$(ls "$MNT/v2")
assert_contains     "$l2" 'file.txt' 'v2 still has the file'

# object row still exists
listing=$("$VFS" object list)
assert_contains "$listing" "$ID" 'object row preserved after one mapping removed'

unmount_view "$MNT/v1"
unmount_view "$MNT/v2"
