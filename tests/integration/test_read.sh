#!/usr/bin/env bash
# T10: Reading a visible file.
# T11: Attempting to read a non-visible file and receiving ENOENT.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create v1 >/dev/null

src="$STORE/.scratch-read"
echo 'hello world' > "$src"
"$VFS" object import "$src" >/dev/null
ID=$(first_object_id)
"$VFS" view add v1 "$ID" /file.txt >/dev/null

mount_view v1 "$MNT/v1"

# T10
content=$(cat "$MNT/v1/file.txt")
assert_eq "$content" 'hello world' 'read of visible file'

# T11 — ENOENT for a path not in the view
err=$(cat "$MNT/v1/no-such" 2>&1 || true)
assert_contains "$err" 'No such file' 'ENOENT for non-mapped path'

unmount_view "$MNT/v1"
