#!/usr/bin/env bash
# T12: Writing a shared object through one view and reading the change
#      through another view.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create v1 >/dev/null
"$VFS" view create v2 >/dev/null

src="$STORE/.scratch-cross"
echo 'before' > "$src"
"$VFS" object import "$src" >/dev/null
ID=$(first_object_id)

"$VFS" view add v1 "$ID" /v1-path.txt >/dev/null
"$VFS" view add v2 "$ID" /v2-path.txt >/dev/null

mount_view v1 "$MNT/v1"
mount_view v2 "$MNT/v2"

# write through v1
echo 'AFTER' > "$MNT/v1/v1-path.txt"
sleep 0.3   # give the writing daemon time to NOTIFY + v2's daemon to invalidate

through_v2=$(cat "$MNT/v2/v2-path.txt")
assert_eq "$through_v2" 'AFTER' 'change visible through v2'

unmount_view "$MNT/v1"
unmount_view "$MNT/v2"
