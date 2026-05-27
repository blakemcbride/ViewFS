#!/usr/bin/env bash
# T2: Creating views.
# T4: Adding an object to one view.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create programming >/dev/null
"$VFS" view create writing     >/dev/null

views=$("$VFS" view list)
assert_contains "$views" 'programming' 'programming present in view list'
assert_contains "$views" 'writing'     'writing present in view list'

echo 'data' > "$STORE/.scratch"
"$VFS" object import "$STORE/.scratch" >/dev/null
OBJ=$(first_object_id)

"$VFS" view add programming "$OBJ" /only.txt >/dev/null

show_p=$("$VFS" view show programming)
assert_contains "$show_p" '/only.txt' 'mapping visible in programming'

show_w=$("$VFS" view show writing)
assert_not_contains "$show_w" '/only.txt' 'mapping must not leak to writing'
