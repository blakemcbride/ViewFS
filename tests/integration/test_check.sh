#!/usr/bin/env bash
# `viewfs check` detects DB<->filesystem divergence: a deleted content file
# is reported as missing and the command exits non-zero.
source "$(dirname "$0")/lib.sh"
init_store

"$VFS" view create v >/dev/null
"$VFS" dir mkdir v /d >/dev/null
"$VFS" prop set v:/d k val >/dev/null
printf 'payload\n' > "$STORE/f"
"$VFS" object import "$STORE/f" --into v:/d >/dev/null
id=$("$VFS" object id v /d/f)

# Healthy store: consistent, exit 0.
out=$("$VFS" check); rc=$?
assert_eq "$rc" "0" "healthy store: check exits 0"
assert_contains "$out" "Store is consistent." "healthy store reported consistent"
assert_contains "$out" "directories with bad name/parent:   0" "dir structure ok"

# Corrupt the store: remove the object's content blob (sharded by id[0:2]).
shard="${id:0:2}"
rm -f "$STORE/objects/$shard/$id"

set +e
out=$("$VFS" check); rc=$?
set -e
assert_eq "$rc" "1" "corrupted store: check exits 1"
assert_contains "$out" "objects with missing content file:  1" "missing content reported"

echo "PASS: test_check"
