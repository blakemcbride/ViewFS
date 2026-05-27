#!/usr/bin/env bash
# T20: Handling `..` traversal safely.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create v1 >/dev/null

src="$STORE/.scratch-tr"
echo 'inside' > "$src"
"$VFS" object import "$src" >/dev/null
ID=$(first_object_id)
"$VFS" view add v1 "$ID" /sub/file.txt >/dev/null

mount_view v1 "$MNT/v1"

# Inside the mount, .. is resolved by the kernel before paths reach the
# FUSE daemon. /v1/sub/.. should canonicalize to /v1, listing only the
# view's own entries.
ls_top=$(ls "$MNT/v1/sub/..")
assert_contains "$ls_top" 'sub' '..  resolves to view root'

# Trying to access a path that would escape the view returns ENOENT
# (the kernel resolves the escape to a path that has no matching mapping
# inside the view).
err=$(cat "$MNT/v1/sub/../no-such" 2>&1 || true)
assert_contains "$err" 'No such file' 'no escape via ..'

# A canonical path with '/sub' under the mount works as expected.
got=$(cat "$MNT/v1/sub/file.txt")
assert_eq "$got" 'inside' 'mapped file readable'

unmount_view "$MNT/v1"
