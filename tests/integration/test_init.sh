#!/usr/bin/env bash
# init + status + check on a fresh, empty store.
source "$(dirname "$0")/lib.sh"
init_store

status=$("$VFS" status)
assert_contains "$status" "schema_version:   1" "schema version is 1"
assert_contains "$status" "views:            0" "no views yet"
assert_contains "$status" "directories:      0" "no dirs yet"

# A fresh store is consistent.
out=$("$VFS" check)
assert_contains "$out" "Store is consistent." "fresh store consistent"

echo "PASS: test_init"
