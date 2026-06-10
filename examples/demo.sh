#!/usr/bin/env bash
# ViewFS demonstration — the property-driven view model.
#
# A directory carries a set of property/value pairs; a file (object) appears
# in a directory exactly when the object's properties are a SUPERSET of the
# directory's effective set (its own pairs plus any ancestor pair marked to
# "flow" down). Nothing records where a file lives — membership is computed.
#
# Without arguments, the script pauses between steps so a human can read the
# output. Pass --unattended (or -u) to run straight through (used by CI).
#
# Defaults (override via the listed environment variables):
#   VIEWFS_DEMO_STORE   /tmp/viewfs-demo
#   VIEWFS_DEMO_MNT     /tmp/viewfs-demo-mnt
#   VIEWFS_DEMO_PG      host=/var/run/postgresql user=postgres dbname=viewfs
#   VIEWFS_DEMO_SCHEMA  viewfs_demo
#
# The demo writes only inside its store directory, its mount root, and the
# Postgres schema given by VIEWFS_DEMO_SCHEMA (dropped + recreated each run).

set -euo pipefail

UNATTENDED=0
case "${1:-}" in
  --unattended|-u) UNATTENDED=1 ;;
  --help|-h) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  '') ;;
  *) echo "demo.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
esac

STORE="${VIEWFS_DEMO_STORE:-/tmp/viewfs-demo}"
MNT_BASE="${VIEWFS_DEMO_MNT:-/tmp/viewfs-demo-mnt}"
PG="${VIEWFS_DEMO_PG:-host=/var/run/postgresql user=postgres dbname=viewfs}"
DEMO_SCHEMA="${VIEWFS_DEMO_SCHEMA:-viewfs_demo}"

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF_DIR/.." && pwd)
VFS="$ROOT/vfs"
VFS_FUSE="$ROOT/vfs-fuse"

for bin in "$VFS" "$VFS_FUSE"; do
  [[ -x "$bin" ]] || { echo "demo.sh: $bin not found — run 'make' first." >&2; exit 2; }
done

cleanup() {
  fusermount3 -u "$MNT_BASE/docs"    2>/dev/null || true
  fusermount3 -u "$MNT_BASE/archive" 2>/dev/null || true
}
trap cleanup EXIT

step() {
  if [[ -t 1 ]]; then printf '\n\033[1;36m===== %s =====\033[0m\n' "$*"
  else printf '\n===== %s =====\n' "$*"; fi
}
note()  { printf '  %s\n' "$*"; }
run()   { printf '  $ %s\n' "$*"; "$@"; }
pause() { (( UNATTENDED )) && return; printf '\n  [Press Enter to continue]'; read -r _; }

# ----------------------------------------------------------------------
cleanup
rm -rf "$STORE" "$MNT_BASE"
mkdir -p "$MNT_BASE/docs" "$MNT_BASE/archive"

if command -v psql >/dev/null 2>&1; then
  psql "$PG" -tAc "DROP SCHEMA IF EXISTS \"$DEMO_SCHEMA\" CASCADE" >/dev/null \
    || { echo "demo.sh: cannot connect via: psql '$PG'"; exit 2; }
else
  echo "demo.sh: psql not installed" >&2; exit 2
fi

# ----------------------------------------------------------------------
step "Step 1: initialize a backing store"
run "$VFS" init "$STORE" --pg "$PG" --schema "$DEMO_SCHEMA"
export VIEWFS_STORE="$STORE"
pause

step "Step 2: create a view and a directory tree of property filters"
run "$VFS" view create docs 'document library'
note ""
note "Each directory below is just a FILTER. Nothing is placed in them yet."
run "$VFS" dir mkdir docs /by-author
run "$VFS" dir mkdir docs /by-author/blake
run "$VFS" prop set docs:/by-author/blake author blake
run "$VFS" dir mkdir docs /reports
run "$VFS" prop set docs:/reports kind report --flow
run "$VFS" dir mkdir docs /reports/2024
run "$VFS" prop set docs:/reports/2024 year 2024
note ""
note "'kind=report' on /reports is marked --flow, so it cascades to"
note "/reports/2024, whose effective filter is therefore {kind=report, year=2024}:"
run "$VFS" prop list docs:/reports/2024 --effective
pause

step "Step 3: import files; --into DIR gives the object that dir's pairs"
echo 'Q3 revenue summary'  > /tmp/demo-q3.txt
echo 'design notes'        > /tmp/demo-design.txt
echo 'blake 2024 report'   > /tmp/demo-annual.txt
run "$VFS" object import /tmp/demo-q3.txt     --into docs:/reports
run "$VFS" object import /tmp/demo-design.txt --into docs:/by-author/blake
# A file that satisfies BOTH the report-tree filter and the author filter:
ANNUAL=$("$VFS" object import /tmp/demo-annual.txt | awk '{print $1}')
run "$VFS" prop set "$ANNUAL" kind report
run "$VFS" prop set "$ANNUAL" year 2024
run "$VFS" prop set "$ANNUAL" author blake
pause

step "Step 4: membership is COMPUTED from those properties"
note "/reports  (kind=report)  ->  q3 and the annual report:"
run "$VFS" dir ls docs /reports
note ""
note "/reports/2024  (kind=report AND year=2024)  ->  only the annual report:"
run "$VFS" dir ls docs /reports/2024
note ""
note "/by-author/blake  (author=blake)  ->  design notes and the annual report:"
run "$VFS" dir ls docs /by-author/blake
note ""
note "The annual report appears in THREE directories at once — no copies."
pause

step "Step 5: find objects by property (AND across pairs)"
run "$VFS" find --prop kind=report --prop year=2024
pause

step "Step 6: mount the view and browse it like a normal filesystem"
run "$VFS" mount docs "$MNT_BASE/docs"
sleep 0.3
note "the view root has an empty filter, so it lists every object:"
run ls "$MNT_BASE/docs"
note ""
note "/reports/2024 through the kernel:"
run ls "$MNT_BASE/docs/reports/2024"
run cat "$MNT_BASE/docs/reports/2024/demo-annual.txt"
pause

step "Step 7: create a file inside a filter — it takes on that dir's pairs"
note 'writing a new file into /by-author/blake ...'
echo 'fresh idea' > "$MNT_BASE/docs/by-author/blake/idea.txt"
NEW=$("$VFS" object id docs /by-author/blake/idea.txt)
note "the new object automatically gained the directory's properties:"
run "$VFS" prop list "$NEW"
pause

step "Step 8: a SECOND view re-filters the very same objects"
run "$VFS" view create archive 'everything from 2024'
run "$VFS" dir mkdir archive /2024
run "$VFS" prop set archive:/2024 year 2024
run "$VFS" mount archive "$MNT_BASE/archive"
sleep 0.3
note "archive:/2024 (year=2024) surfaces the annual report — same object,"
note "no placement, just a different filter over shared properties:"
run ls "$MNT_BASE/archive/2024"
pause

step "Step 9: unmount, remount — everything persists"
"$VFS" unmount "$MNT_BASE/docs"
"$VFS" unmount "$MNT_BASE/archive"
sleep 0.3
"$VFS" mount docs    "$MNT_BASE/docs"
"$VFS" mount archive "$MNT_BASE/archive"
sleep 0.3
run ls "$MNT_BASE/docs/reports/2024"
run "$VFS" status
run "$VFS" check

echo
echo '*** Demonstration complete. ***'
echo '    Run again any time; the demo wipes its own schema and store first.'
