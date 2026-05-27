#!/usr/bin/env bash
# T1: Initializing a backing store.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store

[[ -f "$STORE/config.toml" ]] || { echo 'config.toml missing'; exit 1; }
[[ -d "$STORE/objects"     ]] || { echo 'objects/ missing';   exit 1; }
[[ -d "$STORE/tmp"         ]] || { echo 'tmp/ missing';       exit 1; }
[[ -d "$STORE/daemons"     ]] || { echo 'daemons/ missing';   exit 1; }
[[ -d "$STORE/logs"        ]] || { echo 'logs/ missing';      exit 1; }

# schema_migrations row at version 1
v=$(psql "$PG" -tAc "SELECT max(version) FROM \"$TEST_SCHEMA\".schema_migrations")
assert_eq "$v" "1" "schema_migrations.version"
