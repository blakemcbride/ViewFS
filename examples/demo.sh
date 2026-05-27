#!/usr/bin/env bash
# ViewFS demonstration — implements spec §18 step-by-step.
#
# Without arguments, the script pauses between major steps so a human can
# read the output. Pass --unattended (or -u) to run it through without
# stopping, which is what CI uses.
#
# Defaults (override via the listed environment variables):
#   VIEWFS_DEMO_STORE   /tmp/viewfs-demo
#   VIEWFS_DEMO_MNT     /tmp/viewfs-demo-mnt
#   VIEWFS_DEMO_PG      host=/var/run/postgresql user=postgres dbname=viewfs
#   VIEWFS_DEMO_SCHEMA  viewfs_demo
#
# The demo writes only inside its store directory, its mount root, and the
# Postgres schema given by VIEWFS_DEMO_SCHEMA (which is dropped + recreated
# on every run for idempotency).

set -euo pipefail

UNATTENDED=0
case "${1:-}" in
  --unattended|-u) UNATTENDED=1 ;;
  --help|-h)
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 0 ;;
  '') ;;
  *)
    echo "demo.sh: unknown argument '$1' (try --help)" >&2
    exit 2 ;;
esac

STORE="${VIEWFS_DEMO_STORE:-/tmp/viewfs-demo}"
MNT_BASE="${VIEWFS_DEMO_MNT:-/tmp/viewfs-demo-mnt}"
PG="${VIEWFS_DEMO_PG:-host=/var/run/postgresql user=postgres dbname=viewfs}"
DEMO_SCHEMA="${VIEWFS_DEMO_SCHEMA:-viewfs_demo}"

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF_DIR/.." && pwd)
VFS="$ROOT/viewfs"
VFS_FUSE="$ROOT/viewfs-fuse"

for bin in "$VFS" "$VFS_FUSE"; do
  if [[ ! -x "$bin" ]]; then
    echo "demo.sh: $bin not found — run 'make' from the project root first." >&2
    exit 2
  fi
done

cleanup() {
  fusermount3 -u "$MNT_BASE/programming" 2>/dev/null || true
  fusermount3 -u "$MNT_BASE/writing"     2>/dev/null || true
}
trap cleanup EXIT

step() {
  if [[ -t 1 ]]; then
    printf '\n\033[1;36m===== %s =====\033[0m\n' "$*"
  else
    printf '\n===== %s =====\n' "$*"
  fi
}

note() { printf '  %s\n' "$*"; }

pause() {
  (( UNATTENDED )) && return
  printf '\n  [Press Enter to continue]'
  read -r _
}

# ----------------------------------------------------------------------
# Pre-flight
# ----------------------------------------------------------------------
cleanup
rm -rf "$STORE" "$MNT_BASE"
mkdir -p "$MNT_BASE/programming" "$MNT_BASE/writing"

# Drop the demo PG schema so the demo is idempotent. We use a dedicated
# schema name so this never touches a production 'viewfs' schema.
if command -v psql >/dev/null 2>&1; then
  psql "$PG" -tAc "DROP SCHEMA IF EXISTS \"$DEMO_SCHEMA\" CASCADE" >/dev/null \
    || {
      echo "demo.sh: could not connect to Postgres via:"
      echo "    psql '$PG'"
      echo "Adjust VIEWFS_DEMO_PG to point at a reachable server."
      exit 2
    }
else
  echo "demo.sh: psql not installed; please install postgresql client" >&2
  exit 2
fi

# ----------------------------------------------------------------------
# Steps 1–14 (spec §18)
# ----------------------------------------------------------------------

step "Step 1: initialize a new backing store"
"$VFS" init "$STORE" --pg "$PG" --schema "$DEMO_SCHEMA"
export VIEWFS_STORE="$STORE"
pause

step "Step 2: create two views"
"$VFS" view create programming 'source code'
"$VFS" view create writing     'docs and notes'
"$VFS" view list
pause

step "Step 3: import three files"
echo 'int main(void){return 0;}' > /tmp/demo-foo.c
echo '# Article notes'           > /tmp/demo-notes.md
echo '# Shared README'           > /tmp/demo-readme.md
"$VFS" object import /tmp/demo-foo.c
"$VFS" object import /tmp/demo-notes.md
"$VFS" object import /tmp/demo-readme.md
FOO=$(   "$VFS" object list | awk '/demo-foo/   {print $1; exit}')
NOTES=$( "$VFS" object list | awk '/demo-notes/ {print $1; exit}')
README=$("$VFS" object list | awk '/demo-readme/{print $1; exit}')
note "foo.c     id = $FOO"
note "notes.md  id = $NOTES"
note "readme.md id = $README"
pause

step "Step 4: programming only — foo.c at /src/foo.c"
"$VFS" view add programming "$FOO" /src/foo.c
"$VFS" view show programming
pause

step "Step 5: writing only — notes.md at /articles/notes.md"
"$VFS" view add writing "$NOTES" /articles/notes.md
"$VFS" view show writing
pause

step "Step 6: shared README under different paths in each view"
"$VFS" view add programming "$README" /README.md
"$VFS" view add writing     "$README" /docs/readme.md
note "all view paths pointing at the shared object:"
"$VFS" object paths "$README"
pause

step "Step 7: mount both views simultaneously"
"$VFS" mount programming "$MNT_BASE/programming"
"$VFS" mount writing     "$MNT_BASE/writing"
sleep 0.3
mount | awk -v m="$MNT_BASE" '$0 ~ m'
pause

step "Step 8: each view lists only its mapped files"
note "programming:"
ls -l "$MNT_BASE/programming"
echo
note "writing:"
ls -l "$MNT_BASE/writing"
pause

step "Step 9: modify the shared file through the programming view"
note "before:"
cat "$MNT_BASE/programming/README.md"
echo 'EDITED THROUGH THE PROGRAMMING VIEW' > "$MNT_BASE/programming/README.md"
note "after:"
cat "$MNT_BASE/programming/README.md"
pause

step "Step 10: the writing view sees the same change"
cat "$MNT_BASE/writing/docs/readme.md"
pause

step "Step 11: rename the shared file in programming only"
mv "$MNT_BASE/programming/README.md" "$MNT_BASE/programming/README-renamed.md"
note "programming sees the new name:"
ls "$MNT_BASE/programming"
pause

step "Step 12: writing's path is unchanged"
note "writing still has /docs/readme.md:"
ls "$MNT_BASE/writing/docs"
note ""
note "object paths now show the rename in programming, untouched in writing:"
"$VFS" object paths "$README"
pause

step "Step 13: unmount, then remount"
"$VFS" unmount "$MNT_BASE/programming"
"$VFS" unmount "$MNT_BASE/writing"
sleep 0.3
note "both views unmounted; remounting..."
"$VFS" mount programming "$MNT_BASE/programming"
"$VFS" mount writing     "$MNT_BASE/writing"
sleep 0.3
pause

step "Step 14: mappings persist across remount"
note "programming:"
ls "$MNT_BASE/programming"
echo
note "writing:"
ls "$MNT_BASE/writing/docs"
echo
note "and the content from step 9 is still there:"
cat "$MNT_BASE/writing/docs/readme.md"

echo
echo '*** Demonstration complete. ***'
echo '    Run again any time; the demo wipes its own schema and store first.'
