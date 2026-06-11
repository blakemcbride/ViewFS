#!/usr/bin/env bash
# Mounted read path: ls shows child dirs + computed members; cat works;
# --ro is enforced; the empty-filter root matches every object.
source "$(dirname "$0")/lib.sh"
init_store

"$VFS" view create v >/dev/null
"$VFS" dir mkdir v /blake >/dev/null
"$VFS" prop set author=blake v:/blake >/dev/null
printf 'alpha\n' > "$STORE/a.txt"
"$VFS" object import "$STORE/a.txt" --into v:/blake >/dev/null

mount_view v "$MNT/v"

# Root has an empty filter -> matches all objects (a.txt) plus the /blake dir.
root_ls=$(ls "$MNT/v")
assert_contains "$root_ls" "blake" "root lists the directory"
assert_contains "$root_ls" "a.txt" "root (empty filter) lists every object"

# /blake lists the matching file.
assert_contains "$(ls "$MNT/v/blake")" "a.txt" "/blake lists a.txt"
assert_eq "$(cat "$MNT/v/blake/a.txt")" "alpha" "cat returns content"

unmount_view "$MNT/v"

# Read-only mount rejects writes.
mount_view v "$MNT/v"   # (re-mount; mount_view uses default rw)
unmount_view "$MNT/v"
"$VFS" mount v "$MNT/v" --ro
i=0; while (( i<50 )); do mount_in_procmounts "$MNT/v" && break; sleep 0.1; i=$((i+1)); done
assert_eq "$(cat "$MNT/v/blake/a.txt")" "alpha" "ro: read still works"
expect_failure bash -c "echo x > '$MNT/v/blake/new.txt'"
unmount_view "$MNT/v"

echo "PASS: test_mount_read"
