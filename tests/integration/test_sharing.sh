#!/usr/bin/env bash
# T5: Adding the same object to two views.
# T6: Assigning different paths to the same object in different views.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create progA >/dev/null
"$VFS" view create progB >/dev/null

src="$STORE/.scratch-shared"
echo 'shared-content' > "$src"
"$VFS" object import "$src" >/dev/null
ID=$(first_object_id)

# T5 + T6
"$VFS" view add progA "$ID" /a/foo.txt          >/dev/null
"$VFS" view add progB "$ID" /b/different-name.c >/dev/null

paths=$("$VFS" object paths "$ID")
assert_contains "$paths" 'progA:/a/foo.txt'
assert_contains "$paths" 'progB:/b/different-name.c'
