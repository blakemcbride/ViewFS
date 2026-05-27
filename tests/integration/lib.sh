# Shared helpers for ViewFS integration tests. Sourced by each test_*.sh.
#
# A sourcing test gets:
#   - $VFS, $VFS_FUSE     paths to the binaries
#   - $PG                 libpq conninfo
#   - $TEST_SCHEMA        unique PG schema (dropped on exit)
#   - $STORE              isolated backing-store directory (rm'd on exit)
#   - $MNT                a directory for mountpoints (rm'd on exit)
#   - VIEWFS_STORE set after init_store
#
# And these functions:
#   init_store            initializes a fresh store with $TEST_SCHEMA
#   mount_view V DIR      mounts view V at DIR, waiting for it to be live
#   unmount_view DIR      unmounts and waits for /proc/mounts to drop the entry
#   assert_eq A B [msg]
#   assert_contains H N [msg]
#   assert_not_contains H N [msg]
#   expect_failure CMD... runs CMD; passes iff it exits non-zero
#   first_object_id       prints the id from the first row of `viewfs object list`
#   object_id_matching P  prints the id matching pattern P (basename grep)

set -euo pipefail

TEST_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
VFS="${VFS:-$TEST_ROOT/viewfs}"
VFS_FUSE="${VFS_FUSE:-$TEST_ROOT/viewfs-fuse}"
PG="${VIEWFS_TEST_PG:-host=/var/run/postgresql user=postgres dbname=viewfs}"

if [[ ! -x "$VFS" || ! -x "$VFS_FUSE" ]]; then
  echo "lib.sh: $VFS or $VFS_FUSE missing — run 'make' first." >&2
  exit 2
fi

if ! command -v psql >/dev/null 2>&1; then
  echo "lib.sh: psql not installed" >&2
  exit 2
fi
if ! psql "$PG" -tAc 'SELECT 1' >/dev/null 2>&1; then
  echo "lib.sh: cannot reach Postgres via '$PG' (set VIEWFS_TEST_PG)" >&2
  exit 2
fi

TEST_SCHEMA="viewfs_test_$$_$RANDOM"
STORE="/tmp/viewfs-test-$$-$RANDOM"
MNT="$STORE-mnt"

cleanup() {
  local rc=$?
  if [[ -d "$MNT" ]]; then
    for d in "$MNT"/*; do
      [[ -d "$d" ]] || continue
      fusermount3 -u "$d" >/dev/null 2>&1 || true
    done
  fi
  psql "$PG" -tAc "DROP SCHEMA IF EXISTS \"$TEST_SCHEMA\" CASCADE" \
       >/dev/null 2>&1 || true
  rm -rf "$STORE" "$MNT"
  exit "$rc"
}
trap cleanup EXIT

mkdir -p "$MNT"

init_store() {
  "$VFS" init "$STORE" --pg "$PG" --schema "$TEST_SCHEMA" >/dev/null
  export VIEWFS_STORE="$STORE"
}

# wait_proc_mounts CMD MOUNT_DIR — succeed when the predicate over the
# entry in /proc/mounts holds. CMD is "has" or "gone".
mount_in_procmounts() {
  grep -qE "[[:space:]]$1[[:space:]]" /proc/mounts 2>/dev/null
}

mount_view() {
  local view=$1
  local mountpoint=$2
  mkdir -p "$mountpoint"
  "$VFS" mount "$view" "$mountpoint"
  local i=0
  while (( i < 50 )); do
    if mount_in_procmounts "$mountpoint"; then return 0; fi
    sleep 0.1
    i=$((i + 1))
  done
  echo "FAIL: mount of view '$view' at $mountpoint did not come up" >&2
  return 1
}

unmount_view() {
  local mountpoint=$1
  "$VFS" unmount "$mountpoint" >/dev/null 2>&1 \
    || fusermount3 -u "$mountpoint" >/dev/null 2>&1 \
    || true
  # Poll for up to 10 seconds. The actual unmount usually completes in
  # under 50 ms; the long ceiling exists because the daemon's notify
  # thread has a 1-second poll cadence on shutdown.
  local i=0
  while (( i < 100 )); do
    mount_in_procmounts "$mountpoint" || return 0
    # Re-issue the unmount every 500 ms in case the first call lost a
    # race with the kernel (rare but observed under load).
    if (( i > 0 && i % 5 == 0 )); then
      fusermount3 -u "$mountpoint" >/dev/null 2>&1 || true
    fi
    sleep 0.1
    i=$((i + 1))
  done
  echo "FAIL: $mountpoint did not unmount" >&2
  return 1
}

assert_eq() {
  local got="$1" want="$2" msg="${3:-mismatch}"
  if [[ "$got" != "$want" ]]; then
    echo "FAIL: $msg" >&2
    echo "  got:  $got" >&2
    echo "  want: $want" >&2
    exit 1
  fi
}

assert_contains() {
  local haystack="$1" needle="$2" msg="${3:-substring not found}"
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "FAIL: $msg" >&2
    echo "  needle: $needle" >&2
    echo "  in:     $haystack" >&2
    exit 1
  fi
}

assert_not_contains() {
  local haystack="$1" needle="$2" msg="${3:-substring unexpectedly present}"
  if [[ "$haystack" == *"$needle"* ]]; then
    echo "FAIL: $msg" >&2
    echo "  needle: $needle" >&2
    echo "  in:     $haystack" >&2
    exit 1
  fi
}

expect_failure() {
  if "$@" >/dev/null 2>&1; then
    echo "FAIL: expected '$*' to fail but it succeeded" >&2
    exit 1
  fi
}

first_object_id() {
  "$VFS" object list | awk 'NR==1{print $1; exit}'
}

object_id_matching() {
  local pat=$1
  "$VFS" object list | awk -v pat="$pat" '$0 ~ pat {print $1; exit}'
}
