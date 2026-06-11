#!/usr/bin/env bash
# Mounted write path: create (gains dir props), cp into a dir, mkdir, mv
# (property swap), rm (deletes object), and persistence across remount.
source "$(dirname "$0")/lib.sh"
init_store

"$VFS" view create v >/dev/null
"$VFS" dir mkdir v /blake >/dev/null
"$VFS" prop set author=blake v:/blake --flow >/dev/null
"$VFS" dir mkdir v /blake/2024 >/dev/null
"$VFS" prop set year=2024 v:/blake/2024 >/dev/null

mount_view v "$MNT/v"

# create: a new file in /blake gains author=blake and appears there.
echo "hello" > "$MNT/v/blake/note.txt"
assert_contains "$(ls "$MNT/v/blake")" "note.txt" "created file appears in /blake"
assert_eq "$(cat "$MNT/v/blake/note.txt")" "hello" "created content reads back"
nid=$("$VFS" object id v /blake/note.txt)
assert_contains "$("$VFS" prop list "$nid")" "author" "created file gained author"

# cp a host file into /blake/2024 -> gains year + flowed author.
echo "ext" > "$STORE/ext.txt"
cp "$STORE/ext.txt" "$MNT/v/blake/2024/ext.txt"
eid=$("$VFS" object id v /blake/2024/ext.txt)
props=$("$VFS" prop list "$eid")
assert_contains "$props" "year"   "cp'd file has year"
assert_contains "$props" "author" "cp'd file has flowed author"

# mkdir through the mount.
mkdir "$MNT/v/blake/sub"
assert_contains "$(ls "$MNT/v/blake")" "sub" "mkdir created dir"

# mv note.txt /blake -> /blake/2024 : it gains year=2024.
mv "$MNT/v/blake/note.txt" "$MNT/v/blake/2024/note.txt"
assert_contains "$(ls "$MNT/v/blake/2024")" "note.txt" "moved file in dest"
assert_contains "$("$VFS" prop list "$nid")" "year" "moved file gained dest dir's year"
# (note still shows in /blake too, because author=blake flows into /blake/2024
#  making the child a subset of the parent — that is correct.)

# rm deletes the object outright.
rm "$MNT/v/blake/2024/ext.txt"
assert_not_contains "$(ls "$MNT/v/blake/2024")" "ext.txt" "rm removed file"
assert_eq "$("$VFS" object list | grep -c "$eid" || true)" "0" "rm deleted the object"

unmount_view "$MNT/v"

# Persistence: remount and confirm the surviving file is intact.
mount_view v "$MNT/v"
assert_eq "$(cat "$MNT/v/blake/2024/note.txt")" "hello" "content survives remount"
unmount_view "$MNT/v"

assert_contains "$("$VFS" check)" "Store is consistent." "store consistent after writes"
echo "PASS: test_mount_write"
