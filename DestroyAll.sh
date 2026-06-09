#!/usr/bin/env bash
# DestroyAll.sh -- erase the ViewFS object store + Postgres database. It tears the
# system down only; it does NOT create anything. Run `viewfs init` yourself
# to start fresh (init recreates the database and schema for you).
#
# It (1) unmounts any live ViewFS FUSE mounts and stops their daemons,
#    (2) drops the Postgres database (WITH FORCE, via a maintenance
#    connection), and (3) deletes the object store directory.
#
# Configurable via environment:
#   VIEWFS_STORE     object store directory   (REQUIRED)
#   VIEWFS_DB        database to drop          (default: viewfs)
#   VIEWFS_PG_MAINT  maintenance conninfo      (default: host=/var/run/postgresql user=postgres dbname=postgres)
#
# Usage: ./DestroyAll.sh [-y|--yes] [-h|--help]
#   -y, --yes   skip the confirmation prompt

set -eu

STORE="${VIEWFS_STORE:-}"
DB="${VIEWFS_DB:-viewfs}"
MAINT="${VIEWFS_PG_MAINT:-host=/var/run/postgresql user=postgres dbname=postgres}"

ASSUME_YES=0
case "${1:-}" in
  -y|--yes)  ASSUME_YES=1 ;;
  -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  '')        ;;
  *)         echo "DestroyAll.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
esac

# --- safety guards ---------------------------------------------------------
if [ -z "$STORE" ]; then
  echo "DestroyAll.sh: VIEWFS_STORE is not set." >&2
  echo "          export VIEWFS_STORE=/path/to/store  (or set it inline)." >&2
  exit 2
fi
case "$STORE" in
  /*) ;;
  *)  echo "DestroyAll.sh: VIEWFS_STORE must be an absolute path: '$STORE'" >&2; exit 2 ;;
esac
if [ "$STORE" = "/" ] || [ "$STORE" = "$HOME" ]; then
  echo "DestroyAll.sh: refusing to operate on store path '$STORE'" >&2; exit 2
fi
# The database name is interpolated into SQL; require a plain identifier and
# refuse the built-in maintenance databases.
case "$DB" in [A-Za-z_]*) ;; *) echo "DestroyAll.sh: invalid database name '$DB'" >&2; exit 2 ;; esac
if printf '%s' "$DB" | grep -q '[^A-Za-z0-9_]'; then
  echo "DestroyAll.sh: invalid database name '$DB'" >&2; exit 2
fi
case "$DB" in
  postgres|template0|template1)
    echo "DestroyAll.sh: refusing to drop built-in database '$DB'" >&2; exit 2 ;;
esac

if ! command -v psql >/dev/null 2>&1; then
  echo "DestroyAll.sh: psql not installed." >&2; exit 2
fi
if ! psql "$MAINT" -tAc 'SELECT 1' >/dev/null 2>&1; then
  echo "DestroyAll.sh: cannot reach Postgres via maintenance conninfo: psql '$MAINT'" >&2
  echo "          set VIEWFS_PG_MAINT to a reachable conninfo." >&2
  exit 2
fi

# --- confirm ---------------------------------------------------------------
if [ "$ASSUME_YES" -ne 1 ]; then
  echo "This will DESTROY (and NOT recreate):"
  echo "  object store : $STORE"
  echo "  PG database  : \"$DB\"   (dropped via: $MAINT)"
  echo "and unmount any live ViewFS mounts."
  printf "Proceed? [y/N] "
  read -r ans
  case "$ans" in y|Y|yes|YES) ;; *) echo "aborted."; exit 1 ;; esac
fi

# --- 1. unmount live ViewFS mounts + stop daemons --------------------------
# The daemon mounts with fsname "viewfs:<view>", so its /proc/mounts device
# column starts with "viewfs:". Unmount every such mount.
if [ -r /proc/mounts ]; then
  while read -r dev mnt _rest; do
    case "$dev" in
      viewfs:*)
        echo "unmounting $mnt ($dev)"
        fusermount3 -u "$mnt" 2>/dev/null \
          || fusermount3 -u -z "$mnt" 2>/dev/null \
          || true ;;
    esac
  done < /proc/mounts
fi
# Stop any daemons that recorded a PID in this store.
if [ -d "$STORE/daemons" ]; then
  for pf in "$STORE/daemons"/*.pid; do
    [ -e "$pf" ] || continue
    pid=$(cat "$pf" 2>/dev/null || true)
    [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null || true
  done
fi
sleep 0.3

# --- 2. drop the database --------------------------------------------------
# DROP DATABASE must run from another database; WITH (FORCE) terminates any
# remaining connections (PostgreSQL 13+).
psql "$MAINT" -v ON_ERROR_STOP=1 -tAc \
  "DROP DATABASE IF EXISTS \"$DB\" WITH (FORCE)" >/dev/null
echo "dropped Postgres database \"$DB\""

# --- 3. delete the object store --------------------------------------------
rm -rf "$STORE"
echo "removed object store $STORE"

echo
echo "Old ViewFS system erased. Nothing was recreated."
echo "Run 'viewfs init' when you want a fresh store; it will recreate the"
echo "database and schema."
