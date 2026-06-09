#!/usr/bin/env bash
# Power-loss resilience: a write whose close(2) returned 0, followed by a
# hard kill -9 of the daemon, leaves a consistent store and the bytes intact
# on remount. (Content path + checksum machinery are unchanged from Phase 9.)
source "$(dirname "$0")/lib.sh"
init_store

"$VFS" view create v >/dev/null
"$VFS" dir mkdir v /d >/dev/null
"$VFS" prop set v:/d k val >/dev/null

mount_view v "$MNT/v"

# echo + sync so close(2) has returned and op_flush has run before the kill.
echo "durable-bytes" > "$MNT/v/d/f.txt"
sync

pidfile="$STORE/daemons/v.pid"
assert_eq "$([[ -f "$pidfile" ]] && echo yes)" "yes" "daemon wrote a pid file"
kill -9 "$(cat "$pidfile")" 2>/dev/null || true

# The mount is now stale; lazy-unmount it.
fusermount3 -u -z "$MNT/v" >/dev/null 2>&1 || fusermount3 -u "$MNT/v" >/dev/null 2>&1 || true
sleep 0.5

assert_contains "$("$VFS" check)" "Store is consistent." "store consistent after kill -9"

# Remount and confirm the bytes survived.
mount_view v "$MNT/v"
assert_eq "$(cat "$MNT/v/d/f.txt")" "durable-bytes" "bytes survived the crash"
unmount_view "$MNT/v"

echo "PASS: test_crash"
