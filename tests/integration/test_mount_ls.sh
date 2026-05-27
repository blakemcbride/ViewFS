#!/usr/bin/env bash
# T7:  Mounting a view.
# T8:  Listing visible files.
# T9:  Confirming invisible files are not listed.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create v1 >/dev/null
"$VFS" view create v2 >/dev/null

src1="$STORE/.in-v1"
src2="$STORE/.in-v2-only"
echo 'a' > "$src1"
echo 'b' > "$src2"
"$VFS" object import "$src1" >/dev/null
"$VFS" object import "$src2" >/dev/null
A=$(object_id_matching '\.in-v1$')
B=$(object_id_matching '\.in-v2-only')

"$VFS" view add v1 "$A" /file1.txt   >/dev/null
"$VFS" view add v2 "$B" /private.txt >/dev/null

mount_view v1 "$MNT/v1"
listing=$(ls -A "$MNT/v1")
assert_contains "$listing" 'file1.txt'   'v1 lists its own file'
assert_not_contains "$listing" 'private.txt' 'v2-only file leaked into v1'
unmount_view "$MNT/v1"
