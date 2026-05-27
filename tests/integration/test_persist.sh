#!/usr/bin/env bash
# T15: Persisting view mappings across unmount and remount.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create v1 >/dev/null

src="$STORE/.scratch-persist"
echo 'persistent' > "$src"
"$VFS" object import "$src" >/dev/null
ID=$(first_object_id)
"$VFS" view add v1 "$ID" /persist.txt >/dev/null

mount_view v1 "$MNT/v1"
content_before=$(cat "$MNT/v1/persist.txt")
assert_eq "$content_before" 'persistent' 'initial read'
unmount_view "$MNT/v1"

# Re-mount
mount_view v1 "$MNT/v1"
content_after=$(cat "$MNT/v1/persist.txt")
assert_eq "$content_after" 'persistent' 'content persists across remount'

# And the mapping is still listed.
show=$("$VFS" view show v1)
assert_contains "$show" '/persist.txt' 'mapping persists in DB'

unmount_view "$MNT/v1"
