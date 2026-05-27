#!/usr/bin/env bash
# Phase 9 crash-recovery test.
#
# Procedure:
#   1. Mount a view.
#   2. Write a file via FUSE; close (so op_flush runs fsync + DB UPDATE).
#   3. kill -9 the daemon process.
#   4. Lazy-unmount the now-defunct FUSE mount.
#   5. Run `viewfs check` against the store.
#
# Expected outcome: the store is reported consistent. The Phase 9 changes
# (fsync in op_flush, fsync parent dir after rename, content-before-commit
# in op_create) ensure that any write whose close(2) returned 0 has been
# durably persisted and matches the DB's recorded size.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
"$VFS" view create v1 >/dev/null

mount_view v1 "$MNT/v1"

# Create + write a file through the FUSE mount.
echo 'persisted before crash' > "$MNT/v1/file.txt"

# Sync the FUSE filesystem so the kernel flushes its dirty buffers to
# the daemon (the daemon then fsync's them to host disk via op_flush).
sync

# Find the daemon's PID and kill -9 it.
pidfile="$STORE/daemons/v1.pid"
[[ -f "$pidfile" ]] || { echo "no PID file at $pidfile"; exit 1; }
daemon_pid=$(cat "$pidfile")

# Sanity: it should still be running before we kill it.
if ! kill -0 "$daemon_pid" 2>/dev/null; then
  echo "daemon $daemon_pid was already dead"; exit 1
fi
kill -9 "$daemon_pid"

# Wait for the kernel to notice the daemon is gone.
i=0
while kill -0 "$daemon_pid" 2>/dev/null; do
  sleep 0.1; i=$((i+1))
  (( i < 50 )) || { echo "daemon $daemon_pid did not die"; exit 1; }
done

# Lazy-unmount to clear the stale FUSE mount that's now pointing at a
# dead userspace daemon.
fusermount3 -uz "$MNT/v1" 2>/dev/null \
  || fusermount3 -u  "$MNT/v1" 2>/dev/null \
  || true

# Give the kernel a beat to drop the mount entry.
i=0
while mount_in_procmounts "$MNT/v1"; do
  sleep 0.1; i=$((i+1))
  (( i < 50 )) || { echo "stale mount not cleared"; exit 1; }
done

# Now exercise viewfs check against the (intentionally) post-crash store.
out=$("$VFS" check)
echo "$out"
assert_contains "$out" 'Store is consistent.' \
                'store should be consistent after a clean close + crash'

# And re-mount + read should see the persisted content.
mount_view v1 "$MNT/v1"
got=$(cat "$MNT/v1/file.txt")
assert_eq "$got" 'persisted before crash' 'content survived the crash'
unmount_view "$MNT/v1"
