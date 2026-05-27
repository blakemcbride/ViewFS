# ViewFS Design

Linux FUSE 3 prototype of a view-based filesystem.
Implementation stack: **C + libfuse3 + libpq + PostgreSQL**.
Spec of record: `viewfs_fuse_prototype_spec.md`.

This document commits to specific implementation choices wherever the spec
leaves them open, and notes alternatives that were considered and rejected.

---

## 1. Architecture overview

Three artifacts, sharing one C library.

```
┌──────────────────┐        ┌──────────────────┐
│   viewfs CLI     │        │  viewfs-fuse     │   one process per
│  (admin tool)    │        │  (FUSE daemon)   │   mounted view
└────────┬─────────┘        └────────┬─────────┘
         │                           │
         │   libviewfs  (metadata + object store API)
         ▼                           ▼
   ┌─────────────────────────┐  ┌────────────────────────┐
   │   PostgreSQL (libpq)    │  │  $STORE on local disk  │
   │   schema = viewfs       │  │  config + content blobs│
   └─────────────────────────┘  └────────────────────────┘
```

- **viewfs-fuse**: long-running, one per mounted view. Maintains a small
  pool of `PGconn` connections (one per FUSE worker thread) and serves
  FUSE callbacks.
- **viewfs CLI**: short-lived. Talks directly to PostgreSQL and the object
  directory. No IPC to running daemons in v1; LISTEN/NOTIFY handles
  cross-process cache invalidation.
- **libviewfs**: the only code that knows the schema and on-disk layout.
  Both binaries link it.

Why one daemon per mounted view rather than one daemon multiplexing many
mountpoints: each FUSE session is naturally one mountpoint; multi-view-in-
one-process buys nothing but complexity for a prototype. Postgres MVCC
plus libpq's per-connection model make cross-process metadata access
straightforward.

---

## 2. Backing store layout

PostgreSQL holds all metadata, so the on-disk store contains only content
blobs and configuration.

```
$STORE/
  config.toml              # store_version, pg conninfo, shard depth, defaults
  objects/
    a1/                    # sharded by first 2 hex chars of object id
      a1f8...e9            # raw content, file name == object id
  tmp/                     # staging dir for atomic content writes
  daemons/
    programming.pid        # written by viewfs-fuse on mount
  logs/
    fuse-programming.log
```

Shard depth 1 (256 directories) keeps directories under ~4k entries for
repositories up to roughly 1M objects — enough for a prototype. The
sharded layout is **never exposed through the mount**.

Example `config.toml`:

```toml
store_version = 1
shard_depth   = 1

[postgres]
conninfo = "host=/var/run/postgresql dbname=viewfs user=blake"
schema   = "viewfs"
```

The PostgreSQL schema is versioned via `schema_migrations`. The current
binary expects version 2:

- `0001_init.sql` — the bulk of the schema (objects, views, mappings,
  attributes, tags) plus the `uid`/`gid`/`checksum` columns that the
  daemon now populates.
- `0002_checksum_state.sql` — adds `objects.checksum_state BYTEA` so
  the FUSE daemon can resume an append-only SHA-256 stream after the
  file is closed and reopened. See `Checksum maintenance` below.

If the `[postgres]` section is omitted, libviewfs falls back to
`PGHOST`/`PGUSER`/`PGDATABASE` etc. from the environment.

---

## 3. PostgreSQL schema

A dedicated namespace (`viewfs` by default) so the prototype DB can coexist
with other applications on the local server. Timestamps are stored as
`BIGINT` nanoseconds-since-epoch rather than `TIMESTAMPTZ` so the C code
can hand back kernel-style `struct timespec` without timezone conversions.

```sql
CREATE SCHEMA IF NOT EXISTS viewfs;
SET search_path TO viewfs;

CREATE TABLE schema_migrations (
  version    INTEGER PRIMARY KEY,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE objects (
  object_id       TEXT PRIMARY KEY,                 -- 32-hex-char random id
  kind            TEXT NOT NULL CHECK (kind IN ('file','symlink')),
  size            BIGINT NOT NULL DEFAULT 0,
  mode            INTEGER NOT NULL,                 -- POSIX mode bits
  uid             INTEGER,                          -- captured from fuse_get_context
  gid             INTEGER,
  ctime_ns        BIGINT NOT NULL,
  mtime_ns        BIGINT NOT NULL,
  atime_ns        BIGINT NOT NULL,
  checksum        TEXT,                             -- SHA-256 hex; NULL on internal write
  checksum_state  BYTEA,                            -- 112-byte SHA256_CTX; resumes append-only updates
  source_path     TEXT,                             -- informational
  symlink_target  TEXT                              -- only when kind='symlink'
);

CREATE TABLE views (
  view_name   TEXT PRIMARY KEY,
  description TEXT,
  ctime_ns    BIGINT NOT NULL,
  mtime_ns    BIGINT NOT NULL
);

CREATE TABLE mappings (
  view_name     TEXT NOT NULL REFERENCES views(view_name) ON DELETE CASCADE,
  view_path     TEXT NOT NULL,                      -- canonical, leading '/'
  parent_path   TEXT NOT NULL,                      -- '' if direct child of /
  name          TEXT NOT NULL,                      -- final component
  entry_kind    TEXT NOT NULL CHECK (entry_kind IN ('file','dir','symlink')),
  object_id     TEXT REFERENCES objects(object_id) ON DELETE SET NULL,
  mode_override INTEGER,                            -- per-mapping mode (dirs)
  ctime_ns      BIGINT NOT NULL,
  mtime_ns      BIGINT NOT NULL,
  PRIMARY KEY (view_name, view_path),
  CHECK ( (entry_kind = 'dir') = (object_id IS NULL) )
);
CREATE INDEX mappings_parent ON mappings(view_name, parent_path);
CREATE INDEX mappings_object ON mappings(object_id);

CREATE TABLE attributes (
  object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,
  key       TEXT NOT NULL,
  value     TEXT NOT NULL,
  ctime_ns  BIGINT NOT NULL,
  mtime_ns  BIGINT NOT NULL,
  PRIMARY KEY (object_id, key)
);
CREATE INDEX attributes_kv ON attributes(key, value);

CREATE TABLE tags (
  object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,
  tag       TEXT NOT NULL,
  ctime_ns  BIGINT NOT NULL,
  PRIMARY KEY (object_id, tag)
);
CREATE INDEX tags_tag ON tags(tag);
```

Design choices worth flagging:

- **Directories are explicit** rows (`entry_kind='dir'`, `object_id IS NULL`).
  Implied-only directories make `mkdir` of an empty directory impossible and
  preclude per-directory mode/mtime. Auto-create missing parents on file
  mapping insert; document this behavior in the README per spec §7.9.
- **Object IDs are stable 128-bit random hex** (32 characters). Not derived
  from content (so writes don't change identity) and not from path (so view
  reorganization doesn't change identity). UUIDv7-style time-ordered ids are
  acceptable but not required.
- **`parent_path` denormalization** is the only schema trick — it lets
  `readdir` be one indexed lookup rather than a `LIKE` scan.
- **Last-mapping behavior**: when deleting a mapping drops an object's refcount
  to zero, the object row and content stay; the object becomes discoverable via
  `viewfs object list --orphaned`. This is the "safer default" the spec asks
  for in §7.7.
- **CHECK constraints** enforce two invariants that the daemon depends on:
  `kind` is one of two values, and `object_id IS NULL` iff the row is an
  explicit directory.

---

## 4. Path resolution

Every path entering libviewfs goes through a single pure canonicalizer:

```
canonicalize("/a//b/./c/../d") -> "/a/b/d"
canonicalize("/a/../..")       -> ERR_PATH_ESCAPE
canonicalize("a/b")            -> ERR_PATH_RELATIVE
canonicalize("/a/\0b")         -> ERR_PATH_BADCHAR
```

Rules: leading `/` required; reject `\0` and embedded `/` inside components;
collapse `//`; drop `.`; pop on `..`, error if popping past root. The error
path is what defeats spec §11.1.1's `..` escape attack — the canonicalizer
never returns a "relative-to-elsewhere" path that the rest of the code might
mishandle.

Lookup is then a single PK fetch:
`SELECT entry_kind, object_id FROM mappings WHERE view_name=$1 AND view_path=$2`.

`readdir` uses the parent index:
`SELECT name, entry_kind, object_id FROM mappings WHERE view_name=$1 AND parent_path=$2`,
plus synthetic `.` and `..` entries.

---

## 5. Daemon internals

### 5.1 libpq connection model

A `PGconn` is not safe to share across threads. The **CLI** is
single-threaded and uses one `PGconn` for the lifetime of the process
(opened via `vfs_store_open`, closed by `vfs_store_close`).

The **FUSE daemon** (Phase 2+) will allocate one `PGconn` per FUSE worker
thread, lazily, kept in thread-local storage. With libfuse3's default
thread pool that's roughly 10 connections — well within a typical local
Postgres `max_connections`.

Every `PGconn` runs `SET search_path TO "<schema>";` once at open time
(the schema is read from `config.toml`). The schema name is validated
against `^[A-Za-z_][A-Za-z0-9_]{0,62}$` before being interpolated.

### 5.2 Prepared statements

All hot-path queries are `PQprepare`d once per connection and called via
`PQexecPrepared` with binary parameters:

- `mapping_by_pk`         — getattr, lookup
- `mapping_by_parent`     — readdir
- `object_by_id`          — stat fillers, open
- `mapping_insert`        — create, mkdir
- `mapping_update_path`   — rename
- `mapping_delete`        — unlink, rmdir
- `object_insert`         — create, import
- `object_size_touch`     — release after write

This saves parse/plan cost on every FUSE call and produces tighter error
diagnostics.

### 5.3 Transactions

- Read-only callbacks (`getattr`, `readdir`, `open`, `read`) use autocommit
  — no explicit BEGIN.
- Mutating callbacks (`create`, `unlink`, `rename`, `mkdir`, `rmdir`,
  `truncate`) wrap their multi-statement sequences in `BEGIN; ... COMMIT;`.
- Isolation level: default `READ COMMITTED`. No read-modify-write crosses
  statement boundaries that needs `SERIALIZABLE`.

### 5.4 Inode cache

Inode numbers are invented per-mount (the spec explicitly allows this in
§9). The daemon keeps a map of `ino_t ↔ (view_path, object_id)` plumbed
through FUSE's `lookup`/`forget` refcount protocol. Stable enough for
shells and editors.

### 5.5 Cache invalidation across processes

PostgreSQL `LISTEN/NOTIFY` cleanly solves the daemon-doesn't-see-CLI-edits
problem:

- Every mutating CLI command issues `NOTIFY viewfs_change, '<view_name>'`
  inside its transaction.
- The daemon dedicates one background thread to `LISTEN viewfs_change` on
  its own `PGconn`. On a notification matching its view it calls
  `fuse_lowlevel_notify_inval_entry()` (or `_inval_inode()`) for paths in
  the affected view, dropping the kernel's dentry/attr cache.

This is optional for v1 correctness (the daemon could always re-query), but
it lets us enable a 1–2 second attribute cache via FUSE mount options
without observing stale state during concurrent CLI mutations.

### 5.6 File I/O is pass-through (with incremental checksum)

`open` resolves the mapping, fetches the object_id, opens
`objects/<aa>/<id>` with the FUSE-supplied flags, and allocates a
`struct vfs_ofile { fd, writable, modified, sha_live, known_size, sha }`
which is stashed (by pointer) in `fi->fh`. `read`, `write`, `truncate`,
`fsync`, `release` operate on `o->fd`.

The checksum maintenance policy is implemented around `vfs_ofile`:

| Event | What happens to the SHA stream |
|---|---|
| `op_create` | Init empty SHA, `sha_live=1`, `known_size=0`. Initial digest + state go straight into the INSERT. |
| `op_open` for write | `try_load_checksum_state` does a single SELECT; if `db.size == fstat.size` and `checksum_state IS NOT NULL`, restore the stream and set `sha_live=1, known_size=db.size`. Otherwise `sha_live=0`. |
| `op_write` at `off == known_size` (pure append) | `SHA256_Update` over the new bytes, advance `known_size`. |
| `op_write` at any other offset | `sha_live=0` (the stored hash can't reflect the new content). |
| `op_truncate` | `sha_live=0` (grow inserts zero bytes we never hashed; shrink removes bytes we did hash). |
| `op_flush` (close) | Fsync, then a single UPDATE: if `sha_live`, write fresh size/mtime/atime/checksum/checksum_state; otherwise NULL both checksum columns. |

The DB is touched twice per open/close cycle (one SELECT at open, one
UPDATE at close) regardless of how many `write(2)`s occurred in
between. An append-in-a-loop pattern like
`for i ...; do echo $i >> log; done` therefore costs O(bytes
appended) total instead of O(bytes × iterations).

`viewfs check --fill-checksums` recomputes hash + state for any file
with `checksum IS NULL`. `viewfs check --verify-checksums` re-hashes
each non-NULL row's content and reports mismatches.

### 5.7 Create flow

```
BEGIN;
  INSERT INTO objects (object_id, kind, size, mode, ...)
    VALUES (new_id, 'file', 0, mode, ...);
  ensure_parent_dirs(view, parent_path);            -- recursive auto-mkdir
  INSERT INTO mappings (view_name, view_path, parent_path, name,
                        entry_kind, object_id, ...)
    VALUES ($1, $2, $3, $4, 'file', new_id, ...);
COMMIT;
create_empty_content_file(new_id);                  -- after commit
open and return fh;
```

If post-commit content-file creation fails, `viewfs check` reports the
object as content-missing and offers cleanup. Acceptable for prototype.

### 5.8 Unlink / rmdir

- `unlink`: delete the mapping row. The object row remains; refcount is
  derived from `COUNT(*)` over mappings.
- `rmdir`: refuse with `ENOTEMPTY` if any mapping has `parent_path` equal
  to this directory's view_path. Otherwise delete the row.

### 5.9 Rename within a view

One UPDATE sets `view_path`, `parent_path`, `name`. If the destination
already exists, a delete-then-update inside one transaction mirrors POSIX
rename semantics (the displaced object may orphan, which is fine).

### 5.10 Connection failure handling

A `PGconn` that returns `CONNECTION_BAD` is closed and re-opened. If
reconnect fails repeatedly the daemon returns `EIO` on FUSE calls but keeps
the mount alive so `fusermount3 -u` works. The README will note that
pulling the Postgres server out from under a mounted view is a "you
shouldn't do this" condition.

---

## 6. CLI surface

A global flag `--store PATH` (or env var `VIEWFS_STORE`) selects the
backing store directory for every command except `init`. `main.c` consumes
`--store` at the top level — it may appear before or after the subcommand
name — and stashes the value in `VIEWFS_STORE` for the rest of the
process, so subcommand parsers never need to know about it.

```
viewfs init STORE_PATH [--pg CONNINFO] [--schema NAME] [--reinit]
viewfs status

viewfs view create NAME ["DESCRIPTION"]
viewfs view list
viewfs view show NAME
viewfs view delete NAME

viewfs mount NAME MOUNTPOINT [--foreground] [--verbose] [--ro]
viewfs unmount MOUNTPOINT                # wraps fusermount3 -u

viewfs object import HOST_PATH [--into VIEW:PATH]...
viewfs object show ID|PREFIX
viewfs object paths ID|PREFIX
viewfs object id VIEW VIEW_PATH
viewfs object list [--orphaned]
viewfs object delete ID|PREFIX
viewfs object delete --orphaned [--dry-run]

viewfs view add VIEW OBJECT VIEW_PATH
viewfs view remove VIEW VIEW_PATH
viewfs view populate VIEW --tag TAG --under VIEW_PATH

viewfs attr set ID KEY VALUE
viewfs attr get ID
viewfs attr remove ID KEY
viewfs tag add ID TAG
viewfs tag remove ID TAG
viewfs tag list ID
viewfs find --tag TAG | --attr KEY[=VALUE]

viewfs check [--fix] [--verbose]
```

Notes:

- `init` is idempotent against an existing Postgres schema
  (`IF NOT EXISTS` everywhere). It refuses to overwrite an existing
  `config.toml` unless `--reinit` is passed. `--pg` is optional; if
  omitted, `cmd_init` synthesizes a conninfo using `VIEWFS_PG_USER`
  (optional, libpq-quoted) and `VIEWFS_PG_DATABASE` (defaults to
  `viewfs` if unset). Any remaining field (host, port, password, ...)
  falls back to libpq's `PGHOST`/`PGPORT`/`PGUSER` env vars or
  compiled-in defaults at open time.
- `object import` reads a host file, generates an object id, copies
  content into `objects/<aa>/<id>` via `tmp/` + atomic `rename`, then
  inserts the object row with that pre-allocated id via
  `vfs_object_insert_existing`, then optionally inserts mapping rows for
  each `--into VIEW:PATH`. If the metadata insert fails after content was
  written, the content file is unlinked to avoid an orphan.
- Prefix matching on object IDs uses range queries against the PK:
  `WHERE object_id >= 'a1f' AND object_id < 'a1g'` (the upper bound is
  built by appending 'g' to the prefix). Ambiguous prefixes return
  `VFS_ERR_AMBIGUOUS`; the CLI surfaces this as a clear error.
- `view add` and `object import --into` auto-create missing parent
  directories (mirrors `mkdir -p`) rather than returning `ENOENT`. Same
  helper is shared between `vfs_mapping_add_file` and
  `vfs_mapping_add_dir`.
- `viewfs object delete` removes the DB row AND the content file. The
  schema's FK is `ON DELETE SET NULL` combined with the CHECK constraint
  `(entry_kind = 'dir') = (object_id IS NULL)`, which together cause the
  DELETE to fail with a constraint violation if any *file* mapping still
  references the object. The libpq error message is forwarded to the
  user. (Cleaning up dangling mappings first is the user's responsibility
  in v1; Phase 6's `viewfs check --fix` will help.)
- `viewfs find --attr KEY` (no `=VALUE`) lists every object that has any
  value for that key. `--attr KEY=VALUE` is the equality form.
- `viewfs view create` accepts an optional positional `"DESCRIPTION"`
  after the view name.

---

## 7. View membership model

Per spec §8.3 the README must declare the model. **The prototype uses:
fully explicit mappings, with attribute-based materialization via
`viewfs view populate`.** `populate` is a one-shot batch insert; no live
queries; no dynamic rule engine. This is the simplest option that
satisfies acceptance criterion 14 and leaves the rule-language door open
for later work.

---

## 8. Security boundaries

What the daemon enforces:

- Canonicalizer rejects any path resolving above `/`, killing `..` escape
  (spec §11.1.1, test #20).
- Mappings are looked up by exact `view_path`. Guessing backing-store paths
  through the mount is impossible because the daemon never opens by path
  string; it opens `objects/<aa>/<object_id>` and `object_id` is only
  obtainable via SQL (spec §11.1.5).
- Symlink targets, when implemented, are canonicalized at creation;
  symlinks with absolute or escaping targets are rejected (spec §11.1.2).
- The daemon refuses to mount over a non-empty directory unless
  `--force-empty` is passed.

What the README must say (spec §11.2, §11.3):

- The backing store and the Postgres role are privileged; do not expose
  them to sandboxed processes.
- FUSE view isolation only constrains accesses through the mountpoint. For
  real process isolation, combine with mount namespaces, bubblewrap, or
  containers.
- The prototype is not a security boundary against the user running the
  daemon.

Permissions (spec §11.4):

- Store POSIX mode bits per object.
- Return them through `getattr`.
- Honor read-only when the mount is `--ro`.
- Preserve executable bits across `object import`.
- Ownership is simplified to the invoking user in v1.

---

## 9. FUSE callbacks

### 9.1 Required (spec §12.1), all implemented

`getattr`, `readdir`, `open`, `read`, `write`, `create`, `mkdir`, `unlink`,
`rmdir`, `rename`, `truncate`, `utimens`, `flush`, `fsync`, `release`.

### 9.2 Recommended (spec §12.2)

- `statfs` — synthetic values derived from object count and content dir
  size on disk.
- `chmod` — updates `objects.mode` or `mappings.mode_override` for
  directories.
- `chown` — no-op on uid/gid but doesn't error.
- `access` — mode-bit check against the invoking user.
- `readlink`, `symlink` — if time permits, with target canonicalization.
- `listxattr`, `getxattr`, `setxattr`, `removexattr` — mapped onto the
  `attributes` table behind a `user.viewfs.` prefix. Last to implement.

### 9.3 Error mapping

Per spec §12.4. The one documented ambiguity: `view add` auto-creates
parent directories rather than returning `ENOENT`.

---

## 10. Logging and diagnostics

- `viewfs-fuse --verbose` enables per-op trace logs to stderr or
  `--log-file PATH`.
- All path-resolution rejections log the input path and the rule that
  fired.
- `viewfs check` runs three phases:
  1. **DB integrity** — orphan objects, mappings with dangling object refs
     (FK should prevent this, but a sanity scan is cheap), CHECK invariant
     violations.
  2. **Content↔object** — every object has a content file of the expected
     size; every content file has an object row.
  3. **Migrations** — `schema_migrations.version` matches the binary's
     expected version.
  `--fix` removes only unambiguous garbage (orphan content files, mappings
  with NULL object refs). Never deletes objects.

---

## 11. Testing

Two tiers:

1. **Unit tests** (libviewfs, exercised by a small C test harness):
   canonicalizer rules, schema migrations, mapping CRUD, orphan detection,
   prefix resolution, populate-by-tag.
2. **Integration tests** (`tests/integration/*.bats` or shell scripts):
   real FUSE mounts in a `$STORE` under `mktemp -d`, exercising every
   numbered test from spec §17. Teardown always runs `fusermount3 -u` and
   removes the temp dir.

The integration harness creates a per-test ephemeral Postgres schema
(`viewfs_test_<pid>_<n>`) inside Blake's local DB, points `search_path` at
it, runs the test, then `DROP SCHEMA ... CASCADE` in teardown. No separate
test database or container required.

---

## 12. Demo script

`examples/demo.sh` executes spec §18 step-by-step under `set -euxo
pipefail`, with `read -r` pauses between steps so it doubles as a live
demo. Output is annotated so a viewer can see "now showing that view A
doesn't see file X" and similar.

---

## 13. Build and dependencies

Fedora dependencies:

```sh
sudo dnf install fuse3 fuse3-devel libpq libpq-devel \
                 gcc make pkgconf-pkg-config
```

`Makefile` uses `pkg-config --cflags --libs fuse3 libpq` for the daemon
and the CLI. C standard: C11. Warnings: `-Wall -Wextra -Wpedantic
-Werror=implicit-function-declaration`.

---

## 14. Repository layout

Actual repository layout after Phase 1:

```
viewfs/
  README.md
  Design.md
  Plan.md
  viewfs_fuse_prototype_spec.md
  Makefile
  src/
    libviewfs/                       # static library, links into both bins
      canonicalize.c                 # vfs_path_canonicalize
      config.c                       # minimal TOML-subset reader/writer
      object_id.c                    # 16-byte getrandom() id, lowercase hex
      object_store.c                 # content blob I/O (tmp+rename)
      store.c                        # store lifecycle + migrations + helpers
      views.c                        # views table CRUD
      objects.c                      # objects table CRUD + prefix resolve
      mappings.c                     # mappings CRUD + ensure_parent_dirs
      attrs.c                        # attributes CRUD
      tags.c                         # tags CRUD
      find.c                         # join queries for find --tag / --attr
      version.c                      # viewfs_version_string, vfs_error_str
      internal.h                     # private declarations
      migrations/
        0001_init.sql                # canonical schema (also embedded in
                                     # store.c — keep them in sync)
    fuse/                            # ./viewfs-fuse
      main.c                         # argv, store/view validation, fuse_main
      ops.{c,h}                      # FUSE callbacks + viewfs_ctx
      conn_pool.{c,h}                # per-thread PGconn TLS
    cli/                             # ./viewfs
      main.c                         # top-level dispatcher + --store handling
      common.{c,h}                   # cli_open_store, cli_take_flag, etc.
      cmd_init.c
      cmd_status.c
      cmd_view.c
      cmd_object.c
      cmd_attr.c
      cmd_tag.c
      cmd_find.c
      cmd_mount.c                    # execvp viewfs-fuse
      cmd_unmount.c                  # execvp fusermount3 -u
      cmd_check.c                    # three-phase consistency scan
  include/viewfs/
    viewfs.h                         # the only public header
  tests/
    unit/
      test_canonicalize.c
      test_object_id.c
    integration/
      lib.sh                         # shared per-test isolation + helpers
      run.sh                         # iterates test_*.sh
      test_*.sh                      # 14 scripts: spec §17 + crash test
  examples/
    demo.sh                          # spec §18 demonstration
```

---

## 15. Phase 9 implementation notes

Power-loss resilience: writes whose `close(2)` returned 0 are durable.

- **fsync + DB UPDATE moved from `op_release` to `op_flush`.** The
  kernel invokes `op_flush` synchronously from `close(2)`, while
  `op_release` runs asynchronously after the file descriptor is fully
  dropped. Doing the fsync in `op_release` meant a `close(2)` that
  returned 0 said nothing about durability. Phase 9 moves the work:
  - `op_flush`: if the fd was opened for write, `fsync(fd)` then
    `sync_object_meta` (UPDATE `objects.size`/`mtime_ns`/`atime_ns`)
    then `notify_parent`. Failure of either step propagates as
    `-errno` to `close(2)`, so userspace knows.
  - `op_release`: just `close(fd)`. No DB calls, no fsync.
  - Side effect: every `close(2)` on a write fd now pays one fsync and
    one DB UPDATE. The Phase 4 NOTIFY-driven invalidation continues to
    work because `op_flush` still emits the parent NOTIFY.
- **Parent-directory fsync after `rename` and content creation.** The
  rename of `tmp/import.XXXXXX` into `objects/<aa>/<id>` is atomic in
  POSIX, but the directory entry naming the file isn't durable until
  the parent directory is fsync'd too. A new helper
  `fsync_parent_dir(path)` in `object_store.c` opens the parent
  `O_RDONLY | O_DIRECTORY`, `fsync`s it, and closes it. Called from:
  - `vfs_content_import_host` after the `rename` into the shard dir.
  - `vfs_content_create_empty` after creating the empty content file.
- **fsync errors are fatal.** The previous "best effort; not fatal for
  prototype" comments in `vfs_content_import_host` are gone. An `fsync`
  failure on the tmp file aborts the import; the helper returns
  `VFS_ERR_IO` and leaves the store unchanged.
- **`op_create` reordered: content before commit.** The old flow was
  BEGIN → INSERT object → INSERT mapping → COMMIT → create content
  file, which left a "row without content" window if the daemon
  crashed after the commit but before the content was on disk. Phase 9
  reverses the order: create + fsync the content file first, then run
  the metadata transaction. Failures past content-creation unlink the
  content. A post-commit crash leaves a consistent state; a mid-commit
  crash leaves an orphan content file that `viewfs check --fix`
  removes.
- **What's still NOT durable.** Writes that haven't reached `close(2)`
  yet sit in the kernel page cache and are lost on power loss. Because
  `op_flush` hasn't run, the DB still reflects the pre-write size, so
  the post-crash state is internally consistent — just at the older
  snapshot. This is the spec §4.2 #10 "in-flight write" carve-out
  we intentionally keep.
- **Performance cost.** One `fsync` per close-of-a-write-fd, one
  directory-fsync per content rename / create, and one short DB UPDATE
  per close. For interactive workloads (editors, shells) the cost is
  negligible. For high-throughput batch writes it would matter; a
  future phase could introduce a `--unsafe-async` mode that restores
  the old behavior, or could batch UPDATEs across many releases.
- **Verification.** `tests/integration/test_crash.sh` mounts a view,
  writes a file via `echo > …` (so `close(2)` returns before we move
  on), `sync(1)`'s, `kill -9`'s the daemon (PID from
  `$STORE/daemons/<view>.pid`), lazy-unmounts the stale FUSE mount,
  runs `viewfs check`, and asserts "Store is consistent." Re-mounting
  and reading the file produces the same bytes that were written
  before the kill. The test passes 5/5 consecutive runs.

## 16. Phase 8 implementation notes

Tests are organized in two tiers, both run by `make test`.

- **Unit tests** (`tests/unit/`) compile against `libviewfs.a` and call
  the public API directly. Phase 8 ships:
  - `test_canonicalize` — every rule from `vfs_path_canonicalize`.
  - `test_object_id` — generation uniqueness and the validator's
    accept/reject set.
- **Integration suite** (`tests/integration/`) exercises the CLI and the
  FUSE daemon against a real local PostgreSQL.
  - `lib.sh` is sourced by every test_*.sh. It allocates a unique
    schema `viewfs_test_$$_$RANDOM`, a unique store directory under
    `/tmp/viewfs-test-*`, and a mountpoint root. A single `trap … EXIT`
    handler unmounts, drops the schema, and removes the temp dirs even
    if the test fails. lib.sh refuses to start if `psql` cannot reach
    `$VIEWFS_TEST_PG` (default `host=/var/run/postgresql user=postgres
    dbname=viewfs`), to make environment problems obvious.
  - `mount_view` / `unmount_view` poll `/proc/mounts` rather than
    sleeping a fixed amount; the unmount waiter re-issues
    `fusermount3 -u` every 500 ms after the first second and times out
    at 10 seconds. This handles the small window in which the daemon's
    notify thread (with its 1-second select cadence) finishes shutdown.
  - `run.sh` runs every `test_*.sh` in alphabetical order, captures
    per-test stdout/stderr to `.logs/`, and emits failure logs after
    the summary line.
- **Coverage**: thirteen scripts collectively touch all 20 numbered
  tests from spec §17 (T1–T20); a fourteenth, `test_crash.sh`, was
  added by Phase 9 to verify post-kill consistency. The full mapping
  table is in the README.
- **Schema isolation**: each test schema is dropped on exit, so the
  suite is safe to run against a Postgres instance that also hosts a
  production `viewfs` schema. The connection still runs as a privileged
  role; the safety boundary is the schema name, not the role.
- **Why bash, not bats or pytest**: the surface under test is shell-
  driven (mount, ls, cat, mv, rm). Doing this in C or Python would add
  abstraction without value. The dependency list (bash, coreutils,
  fusermount3, psql) is what's already needed to run the prototype.

## 17. Phase 6 implementation notes


`viewfs check`, `viewfs status` enhancements, and verbose tracing.

- **`vfs_schema_version(s, *out)`** and **`vfs_count_rows(s, table, *out)`**
  are the new libviewfs helpers used by both `status` and `check`.
  `vfs_count_rows` whitelists `table` against the set of libviewfs
  tables before interpolating it into a `SELECT count(*) FROM <t>`
  (whitelisted strings are safe; non-whitelisted return `BADARGS`).
- **`vfs_check_mappings_bad_objref(...)`** and
  **`vfs_check_mappings_dir_invariant(...)`** return the count of rows
  violating each invariant. In a healthy store both are 0; the FK and
  CHECK constraints make non-zero values impossible barring DB
  tampering, but the scan is cheap and the failure mode is clear.
- **`viewfs status`** now includes the schema version, the binary's
  expected version, mapping count, and a recursive walk of
  `$STORE/objects/` reporting file count + total bytes (with a
  human-readable suffix).
- **`viewfs check`** runs three phases:
  1. *DB integrity* — orphan objects, dangling-object-ref mappings,
     dir-invariant violations.
  2. *Content↔object cross-check* — for every object, stat the content
     file (missing ⇒ `content_missing`; size mismatch ⇒
     `content_size_mismatch`). For every file under `objects/<aa>/`,
     check that an object row exists (`content_orphan_files`).
     Symlink objects are skipped in this phase since they have no
     content file by construction.
  3. *Schema version* — compares DB max(version) to `VIEWFS_SCHEMA_VERSION`.
- **`viewfs check --fix`** removes only `content_orphan_files`. It does
  not delete any object row, any mapping, or any file whose contents
  could represent user data. Reporting still happens for all other
  classes, just without taking action.
- **Exit code policy.** `check` returns 0 iff the store is consistent;
  otherwise 1. Orphan *objects* (rows with no mappings) are reported as
  a count but do not count as a problem — they're the normal
  consequence of `unlink` and are managed via
  `viewfs object delete --orphaned` (Phase 1).
- **Verbose tracing.** Every FUSE callback now emits a `trace()` line
  when `--verbose` is in effect, including the high-frequency
  `op_read`/`op_write` (which include the requested size, offset, and
  outcome bytes). `op_chown` traces a `no-op` marker since it
  intentionally ignores its arguments.

## 18. Phase 5 implementation notes

Symlinks, xattrs, and `viewfs view populate`.

- **Symlinks are objects.** `kind='symlink'` rows in `objects` carry the
  target string in `symlink_target` (a column already provisioned by the
  Phase 1 schema). A view mapping with `entry_kind='symlink'` references
  such an object exactly like a file mapping references a file object.
  Mapping-side per-view symlink targets were rejected to keep object
  identity uniform across views.
- **`op_symlink`** runs in a single transaction: `BEGIN;
  ensure_parent_dirs_pg; INSERT objects (kind='symlink',
  symlink_target=...); INSERT mappings (entry_kind='symlink');
  COMMIT;` plus a `notify_parent` for both the immediate parent and root.
- **`op_readlink`** joins mappings + objects and returns the
  `symlink_target` string. `op_getattr` recognizes `entry_kind='symlink'`,
  sets `S_IFLNK | 0777`, and reports `st_size` from the persisted target
  length (Linux's `ls -l` displays this).
- **Symlink target validation is minimal.** We reject control chars and
  oversize strings. We do *not* enforce that the target resolves inside
  the view: target resolution is performed by the kernel against the
  user's mount namespace, so an absolute target like `/etc/passwd`
  escapes whatever we'd validate. The README calls this out and
  recommends pairing ViewFS with `unshare`/bubblewrap when isolation
  matters.
- **xattrs are stored in the existing `attributes` table.** Only the
  `user.viewfs.` namespace is accepted; everything else returns
  `-ENOTSUP`. The trailing portion is the column-stored `key`. This means
  `./viewfs attr get <obj>` and `getfattr -d` are two surfaces over the
  same rows, which is documented behavior. Values are TEXT, so embedded
  NULs are rejected with `-EINVAL` at `op_setxattr` rather than failing
  silently at the SQL boundary. Directory entries return empty lists /
  `-ENODATA` since the directory mapping carries no object.
- **`XATTR_CREATE` / `XATTR_REPLACE` are honored.** Distinct SQL paths
  per flag: `INSERT ... ON CONFLICT DO NOTHING` (CREATE — returns
  `-EEXIST` on conflict), `UPDATE` (REPLACE — returns `-ENODATA` on
  miss), or the standard `INSERT ON CONFLICT DO UPDATE` upsert.
- **`viewfs view populate`** uses `vfs_find_by_tag` and a CLI-side
  callback. The callback computes the view path as
  `<under>/<basename(source_path)>`, falling back to the object id when
  `source_path` is unset. Three counters are kept: `added`, `skipped`
  (the `VFS_ERR_EXISTS` path), and `failed` (anything else). The CLI
  exits 1 if `failed > 0`, 0 otherwise.

## 19. Phase 4 implementation notes


Cache invalidation via PostgreSQL `LISTEN/NOTIFY`.

- **Channel and payload.** All notifications go on the single channel
  `viewfs_change`. The payload is `<view_name>\t<parent_path>` — a TAB
  separator, no JSON or other framing. `<parent_path>` is the canonical
  view path whose child listing may have changed; `""` means root.
- **Emission, two paths.**
  * **CLI / libviewfs mutations** call `vfs_notify_path(s, view, parent)`
    after a successful commit. Wired into `vfs_mapping_add_file`,
    `vfs_mapping_add_dir`, `vfs_mapping_remove`, `vfs_view_delete`. When
    `parent` is non-empty the helper also emits a second NOTIFY with
    `parent=""` so new top-level dirs (created via `ensure_parent_dirs`)
    refresh `ls /mnt` without needing a separate signal per ancestor.
  * **Daemon worker-thread mutations** call a local `notify_parent(pg,
    parent)` helper in `ops.c`. Wired into `op_create`, `op_unlink`,
    `op_mkdir`, `op_rmdir`, `op_rename`, `op_truncate` (path-based),
    `op_release` (for write closes), `op_utimens`, `op_chmod`.
    `op_release` for a write fd is critical: it invalidates the kernel
    attr cache so a subsequent `cat` sees the new size and content.
- **The listener (`src/fuse/notify.{c,h}`).** A dedicated background
  pthread opens its own `PGconn`, runs `SET search_path` and `LISTEN
  viewfs_change`. The loop does `select(PQsocket, ..., 1s timeout)` to
  stay responsive to a stop flag; when the socket has data it calls
  `PQconsumeInput` + `PQnotifies` and dispatches each payload. If the
  view name in the payload matches `CTX.view_name`, the thread calls
  `fuse_invalidate_path(CTX.fuse_handle, parent_or_root)` and drops the
  kernel dentry cache for that directory's children.
- **`CTX.fuse_handle`** is captured in `op_init` via
  `fuse_get_context()->fuse`. The notify thread is started from `op_init`
  and stopped from `op_destroy`.
- **Cache timeouts.** Phase 4 raises FUSE-side caches to:
  ```
  cfg->attr_timeout     = 2.0;
  cfg->entry_timeout    = 2.0;
  cfg->negative_timeout = 1.0;
  ```
  The 2-second window is a safety net. NOTIFY-driven invalidation
  typically resolves within tens of milliseconds; the measured CLI-add
  visibility on a local Postgres socket is ~35ms.
- **Failure modes.** A failed NOTIFY does not roll back its mutation.
  A failed listener `PGconn` is logged; the daemon falls back on the
  2-second cache timeout. If the listener thread can't be started at
  mount time, `op_init` disables the caches (`= 0`) so correctness is
  preserved at some cost in performance.
- **Race window for self-invalidation.** Between a worker thread's
  `notify_parent(...)` call and the listener thread's
  `fuse_invalidate_path(...)`, there's a short async window. In testing
  with `echo > file; cat file` back-to-back on a fresh mount, the cache
  is invalidated before `cat`'s `getattr`, so the round-trip is correct.
  If a tighter guarantee is ever needed, the next step is a synchronous
  invalidation via a self-pipe + a separate one-shot worker thread.

## 20. Phase 3 implementation notes

Decisions made while building the write path.

- **Full write callback set in `ops.c`.** `create`, `write`, `truncate`,
  `unlink`, `mkdir`, `rmdir`, `rename`, `utimens`, `chmod`, `chown`
  (no-op), `flush`, `fsync`. All check `CTX.read_only` and return
  `-EROFS` when the mount was started with `--ro`.
- **`fi->fh` layout.** Lower 32 bits hold the host fd; bit 32 is a
  write-mode flag. `VFS_FH_FD(fh)` and `VFS_FH_IS_WRITE(fh)` macros are
  used everywhere. The flag lets `op_release` know whether to update
  `objects.size`/`mtime_ns`/`atime_ns` on close — read-only closes skip
  the UPDATE entirely.
- **`op_release` syncs DB metadata from the kernel.** When a write fd
  closes, the daemon runs `fstat(fd)` and copies `st_size`, `st_mtim`,
  and `st_atim` into the `objects` row. The kernel is the source of
  truth for these fields during the open file's lifetime; we just
  persist what it tells us.
- **`op_create`** runs the full insert flow inside one transaction:
  `BEGIN; ensure_parent_dirs_pg; INSERT objects; INSERT mappings;
  COMMIT;` then writes the empty content file via `vfs_content_create_empty`
  and opens it `O_RDWR`. Post-commit content-file failure is reported by
  `viewfs check` (Phase 6); we accept that small window.
- **`ensure_parent_dirs_pg` duplicates the libviewfs helper** but takes
  a raw `PGconn *` so worker threads can use it without the
  single-PGconn `vfs_store` API. The libviewfs version (used by the CLI)
  is untouched. Future cleanup may extract a shared core.
- **`op_rename` handles directory subtrees in one UPDATE.** A move from
  `/a` to `/b` (where `/a` is a directory) runs:
  ```
  UPDATE mappings
     SET view_path   = $new || substr(view_path,   length($old) + 1),
         parent_path = $new || substr(parent_path, length($old) + 1)
   WHERE view_name = $view AND view_path LIKE $old || '/%';
  ```
  before updating the renamed entry's own row, so the PK on the moved
  row doesn't collide with descendants. `RENAME_NOREPLACE` is supported;
  `RENAME_EXCHANGE` is rejected with `-EINVAL`; cross-mount renames
  return `-EXDEV` at the kernel/VFS layer before reaching us. We also
  reject moving a directory into its own descendant with `-EINVAL`.
- **`op_unlink` only touches mappings.** The objects row stays; if the
  refcount drops to zero, the object becomes a Phase 6 orphan candidate.
  The deletion is constrained to `entry_kind <> 'dir'` so a stray
  `unlink` against a directory returns `-EISDIR`.
- **FUSE caches disabled in Phase 3.** `op_init` sets
  `cfg->attr_timeout = cfg->entry_timeout = cfg->negative_timeout = 0`.
  Without this, the kernel happily reads from its cached stat results
  and shows stale sizes after writes. The cost is a `getattr` call per
  `stat(2)`. Phase 4's `LISTEN/NOTIFY` invalidation will let us turn the
  cache back on for performance.
- **`chown` is a no-op.** The prototype assumes the daemon owns its
  entire view; per the spec it's acceptable to simplify ownership in
  v1. We accept the call and return 0 instead of returning `-ENOSYS`,
  so editors that issue `chown` after save (e.g. `cp -p`) don't fail.

## 21. Phase 2 implementation notes

These reflect decisions made while building the read-path FUSE daemon.

- **High-level libfuse3 API, not low-level.** Phase 2's read path uses
  `struct fuse_operations` and `fuse_main`. The kernel handles inode
  caching internally; the per-mount monotonic inode allocator and the
  `lookup`/`forget` refcount table mentioned in Plan §5 are not needed
  until we want `fuse_lowlevel_notify_inval_entry()` in Phase 4. If we
  hit that, we can switch wholesale to the low-level API at that point.
  For Phase 2, kernel-supplied inode numbers are stable enough for `ls`,
  `cat`, editors, and shells.
- **Effective read-only.** `op_open` rejects any access mode other than
  `O_RDONLY` with `-EROFS`, regardless of whether the mount was started
  with `--ro`. Phase 3 will lift this restriction; `--ro` will then keep
  the read-only behavior on demand. The kernel translates `EROFS` from
  `open(2)` cleanly: `echo foo > file` reports "Read-only file system".
- **Per-thread `PGconn` pool.** Each FUSE worker thread lazily acquires a
  `PGconn` on first use, stashed in `pthread_key_t` TLS. The pool's
  destructor closes the connection when the thread exits. The pool's
  conninfo and schema strings are copied at init; the pool itself is
  shared, but the connections never are. Implemented in
  `src/fuse/conn_pool.{c,h}`.
- **Daemon-wide context (`viewfs_ctx`).** A single global `CTX` (defined
  in `ops.c`) holds the store pointer, conn_pool pointer, view name,
  read-only flag, and verbose flag. All fields are read-only after
  `main()` finishes setup, so callbacks read them without locks. There
  is one daemon process per mounted view, so this is one struct per
  process.
- **Daemonization is libfuse's.** `fuse_main` daemonizes (forks) unless
  `-f` is passed. We translate our `--foreground` to `-f`. The
  daemonized child's `init()` callback writes
  `$STORE/daemons/<view>.pid`; `destroy()` removes it. This is the
  cleanest way to capture the post-fork PID without doing the fork
  ourselves.
- **DB queries are inline `PQexecParams` for now.** No prepared
  statements yet — the prepared-statement set described in Design §5.2
  is deferred until a later phase finds it worth the complexity. With
  Phase 2's three hot queries (`mapping_by_pk`, `mapping_by_parent`,
  `object_by_id`), profiling will tell us whether parsing cost matters.
- **Path canonicalization at the boundary.** Every callback runs the
  FUSE-supplied path through `vfs_path_canonicalize` before any DB
  query. The kernel pre-resolves `..` cross-mount-boundary, but we
  defend in depth: any path with embedded control chars, escape via
  `..`, or non-`/`-anchored input is rejected with the appropriate
  `errno`.
- **`viewfs mount` exec model.** The CLI does not fork; it `execvp`s
  the `viewfs-fuse` binary directly. It looks for `viewfs-fuse` next
  to the running `viewfs` binary first (via `/proc/self/exe` +
  `dirname`), falling back to `$PATH`. libfuse then takes over and
  handles backgrounding. `viewfs unmount` is a thin wrapper around
  `fusermount3 -u`.

## 22. Phase 1 implementation notes

These reflect the decisions actually made while building Phase 1. They
override anything earlier in this document that contradicts them.

- **Public API**: callback-style iteration. Every "list" function takes a
  `vfs_*_cb cb` plus a `void *ud` (e.g. `vfs_view_cb`, `vfs_mapping_cb`,
  `vfs_attr_cb`, `vfs_tag_cb`, `vfs_object_cb`). The CLI's print routines
  are the callbacks.
- **Error reporting**: every libviewfs entry point returns `vfs_error`.
  Detailed libpq / filesystem error messages are written to a per-store
  buffer (`vfs_store_last_error`). The CLI uses `cli_perror()` to print
  the symbolic error plus the detail string.
- **TOML config**: a hand-rolled minimal parser supports comments
  (`#`/`;`), `[section]`, integers, and double-quoted strings with `\"`
  and `\\` escapes. Anything else is rejected with `VFS_ERR_CONFIG`. No
  external TOML library is linked.
- **Object id format**: 16 bytes from `getrandom(2)` rendered as 32
  lowercase hex chars. Validated by `vfs_object_id_valid`.
- **Embedded migration SQL**: `src/libviewfs/migrations/0001_init.sql` is
  the canonical human-readable form; an identical string literal lives in
  `store.c` (`MIGRATION_0001`) and is what actually runs. They must be
  kept in sync. Future migrations (0002, 0003, …) may switch to a
  generated header, but a single migration didn't justify codegen.
- **Schema creation**: `CREATE SCHEMA IF NOT EXISTS "<schema>"` is run
  outside any migration, before `SET search_path` and before the
  migration runner. The migration SQL itself is schema-agnostic.
- **Auto-create parents**: `vfs_mapping_add_file` and `vfs_mapping_add_dir`
  share a private `ensure_parent_dirs` helper that walks the path and
  inserts `entry_kind='dir'` rows on conflict-do-nothing. Idempotent.
- **Directory removal**: `vfs_mapping_remove` checks the child count via
  the `mappings_parent` index and refuses with `VFS_ERR_NOTEMPTY` if any
  child mapping exists. `rmdir` semantics will reuse this in Phase 3.
- **Object deletion semantics**: `vfs_object_delete` removes the DB row
  and the content file. The schema's `ON DELETE SET NULL` FK paired with
  the CHECK `(entry_kind = 'dir') = (object_id IS NULL)` means a DELETE
  will fail at the database with a constraint violation if any *file*
  mapping still references the object — which is the safety we want.
  Cleaning up referring mappings first is the user's job until Phase 6's
  `viewfs check --fix` lands.
- **Phase 2/6 stubs**: at Phase 1's freeze point, `mount`, `unmount`,
  and `check` were placeholders that printed "implemented in Phase N"
  and exited 2. Phases 2, 2, and 6 respectively replaced them with real
  implementations; this is recorded here only as a snapshot of how the
  CLI surface evolved.
- **Tests**: `make test` builds and runs everything under `tests/unit/`.
  Phase 1 ships `test_canonicalize` covering the rule set documented in
  §4 (root, dot, double-dot, escapes, relative input, control chars,
  duplicate slashes, deep paths). Integration tests landed in Phase 8.

## 23. Open work

- Decide whether to ship symlink support in v1 or defer to v1.1.
- Decide whether `viewfs check --fix` should attempt to repair partial
  inserts (rare; needs a torn-write reproducer first).
- Optional: native attribute-based view membership (live queries) if it
  proves simpler than the materialized `populate` approach during
  implementation.
- The CLI currently runs as the invoking OS user but connects to Postgres
  with whatever role the conninfo specifies. On Fedora with `trust` auth
  on `local`/`127.0.0.1` this is painless; on stricter deployments the
  README will need to spell out how to set up a dedicated PG role.
