#!/usr/bin/env bash
# T3: Importing objects.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store

src="$STORE/.scratch-import"
echo 'import-content' > "$src"
"$VFS" object import "$src" >/dev/null

listing=$("$VFS" object list)
assert_contains "$listing" "$src" 'source_path recorded'

ID=$(first_object_id)
[[ -f "$STORE/objects/${ID:0:2}/$ID" ]] || {
    echo "content file $STORE/objects/${ID:0:2}/$ID missing" >&2
    exit 1
}

# size in DB matches the source file
db_size=$(psql "$PG" -tAc \
    "SELECT size FROM \"$TEST_SCHEMA\".objects WHERE object_id='$ID'")
disk_size=$(stat -c %s "$src")
assert_eq "$db_size" "$disk_size" 'DB size matches host file size'
