# ViewFS Design

ViewFS uses a **property-driven** membership model: a *view* is a per-view
tree of **directories**, each carrying a set of property filters, and a
directory's file contents are **computed** — the objects whose own
properties are a superset of the directory's *effective* filter. Nothing
stores "object X lives in directory D"; membership is always recomputed.

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
│   vfs (CLI)      │        │   vfs-fuse       │   one process per
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

- **vfs-fuse**: long-running, one per mounted view. Maintains a small
  pool of `PGconn` connections (one per FUSE worker thread) and serves
  FUSE callbacks.
- **vfs (CLI)**: short-lived. Talks directly to PostgreSQL and the object
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
    programming.pid        # written by vfs-fuse on mount
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
binary expects **version 1**:

- `0001_init.sql` — the entire schema: `objects` (with the intrinsic
  `name` column and an inlined `checksum_state BYTEA` so the daemon can
  resume an append-only SHA-256 stream across open/close), `views`, the
  per-view `view_dirs` tree, the multi-valued `object_props`, and
  `view_dir_props` (per-directory filters, each pair carrying a `flow`
  flag). See §3.

If the `[postgres]` section is omitted, libviewfs falls back to
`PGHOST`/`PGUSER`/`PGDATABASE` etc. from the environment.

---

## 3. Data model and schema

A dedicated namespace (`viewfs` by default) so the prototype DB can coexist
with other applications on the local server. Timestamps are stored as
`BIGINT` nanoseconds-since-epoch rather than `TIMESTAMPTZ` so the C code
can hand back kernel-style `struct timespec` without timezone conversions.

### 3.1 Conceptual model

A **view** is a named, per-view tree of **directories**, each with an
attached multiset of `(key, value, flow)` property pairs. An **object** is
a content blob with its own multiset of `(key, value)` properties and an
intrinsic `name`. The files listed in directory `D` of view `V` are exactly
the objects whose property multiset is a **superset** of `D`'s *effective*
property set (D's own pairs ∪ ancestor pairs flagged `flow`). Nothing
records "object X is in directory D" — it is always recomputed. Moving a
file means mutating the object's properties; re-filtering a directory means
mutating the directory's properties.

The model's rules:

1. **Properties fully replace explicit placement.** A directory's contents
   are computed, never stored. There is no `mappings` table.
2. **Directories are explicit; files are implied.** Directories form a
   per-view tree with a single root, created via `mkdir` (through the mount
   or the CLI). Files are never directory rows — they appear by matching.
3. **Each directory carries a set of `(key,value)` pairs.** A freshly
   `mkdir`'d directory has an **empty** set, and the empty set is a subset
   of everything, so an empty-filter directory (e.g. a view root) matches
   **every** object.
4. **Matching is superset with AND semantics.** A file appears in `D` iff
   its properties ⊇ `D`'s effective set. Properties are **multi-valued**:
   a directory pair `author=blake` requires the object have that specific
   `(author, blake)` pair; if a directory lists several values for one key
   the object must have all of them.
5. **Per-pair flow flag, default off.** Each directory pair carries a
   boolean `flow`. `flow=false` constrains only that directory;
   `flow=true` also applies to all descendant directories. A directory's
   *effective* set is its own pairs plus every ancestor pair marked `flow`
   (§3.3).

Four decisions follow from the model and shape the schema:

- **D1 — names are an object attribute.** Because a directory is a query,
  every matching object needs a display name. The object gains an intrinsic
  `name` (basename on import, settable via `vfs object name`). The same
  object shows under the same name in every directory it matches.
- **D2 — same-name collisions get a short id suffix.** When two matching
  objects share a `name` in one directory (or a member clashes with a child
  directory's name), `readdir` shows each as `report.txt~<idprefix>` with
  the shortest distinguishing prefix (min 4 hex); a unique name is shown
  bare. `lookup` resolves the bare form first, then `base~hexprefix`.
- **D3 — `unlink` deletes the object.** `rm <file>` removes the object row
  and its content blob outright (it vanishes from every directory it
  matched). To de-list a file *without* destroying it, change its
  properties (`vfs prop unset`, or `mv` it to a directory it no longer
  matches).
- **D4 — through-the-mount `cp` creates a fresh object** with only the
  destination directory's effective pairs (FUSE's `create`+`write` only see
  new bytes, not the source identity). The lossless copy — duplicate
  content, gain dest-dir pairs, retain the source's other props — is the
  object-level `vfs object copy`. `mv` through the mount *does* get full
  move semantics because FUSE hands the daemon both endpoints.

### 3.2 Schema

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
  name            TEXT NOT NULL DEFAULT '',         -- intrinsic display name (D1)
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

-- Multi-valued object properties. Replaces the old attributes + tags
-- tables; a "tag" is sugar for key='tag'. PK includes value.
CREATE TABLE object_props (
  object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,
  key       TEXT NOT NULL,
  value     TEXT NOT NULL,                          -- '' allowed (valueless property/tag)
  ctime_ns  BIGINT NOT NULL,
  mtime_ns  BIGINT NOT NULL,
  PRIMARY KEY (object_id, key, value)
);
CREATE INDEX object_props_kv ON object_props (key, value);  -- membership-critical

-- Directories ONLY; files are never rows. Replaces `mappings`.
CREATE TABLE view_dirs (
  view_name   TEXT NOT NULL REFERENCES views(view_name) ON DELETE CASCADE,
  dir_path    TEXT NOT NULL,                        -- canonical, leading '/'
  parent_path TEXT NOT NULL,                        -- '' for root's children; root itself is '/'
  name        TEXT NOT NULL,                        -- final component; '' for root
  mode        INTEGER NOT NULL,
  ctime_ns    BIGINT NOT NULL,
  mtime_ns    BIGINT NOT NULL,
  PRIMARY KEY (view_name, dir_path)
);
CREATE INDEX view_dirs_parent ON view_dirs (view_name, parent_path);

-- The property pairs attached to each directory, with the flow flag.
CREATE TABLE view_dir_props (
  view_name TEXT NOT NULL,
  dir_path  TEXT NOT NULL,
  key       TEXT NOT NULL,
  value     TEXT NOT NULL,
  flow      BOOLEAN NOT NULL DEFAULT FALSE,
  ctime_ns  BIGINT NOT NULL,
  mtime_ns  BIGINT NOT NULL,
  PRIMARY KEY (view_name, dir_path, key, value),
  FOREIGN KEY (view_name, dir_path)
      REFERENCES view_dirs(view_name, dir_path) ON DELETE CASCADE
);
CREATE INDEX view_dir_props_flow ON view_dir_props (view_name, flow);
```

`0002_checksum_state.sql` from the original build is folded into `0001`
(the `checksum_state` column lives in `objects`); `VIEWFS_SCHEMA_VERSION`
is `1`. Every view gets a root row `('/', '', '', 0755, …)` created in
`vfs_view_create`, so the root always resolves and `ls /mnt` works on a
brand-new view (it shows every object, since root's effective set is empty).

Design choices worth flagging:

- **Object IDs are stable 128-bit random hex** (32 characters). Not derived
  from content (so writes don't change identity) and not from path (so view
  reorganization doesn't change identity).
- **`parent_path` denormalization** lets a directory's child *directories*
  be found with one indexed lookup rather than a `LIKE` scan; its file
  members come from the membership query (§3.4).
- **`object_props_kv`** is the one hot index: the membership query joins
  through it on `(key,value)`.

### 3.3 Effective property set of a directory

```
effective(V, D) =
      { (k,v) | (V,D,k,v,_)         ∈ view_dir_props }          -- own pairs
    ∪ { (k,v) | (V,A,k,v,flow=true) ∈ view_dir_props,
                A is a strict ancestor of D }                    -- flowed pairs
```

Ancestors are found by walking `parent_path` to `/`, or by `dir_path` being
a path-boundary prefix of `D`. Only `flow=true` ancestor rows matter
(`view_dir_props_flow` index).

### 3.4 The membership query (relational division)

Files in directory `D` of view `V` = objects whose props ⊇ `effective(V,D)`.
With `E` that effective set and `n = |E|`:

```sql
SELECT op.object_id
  FROM object_props op
  JOIN (VALUES (k1,v1), …, (kn,vn)) AS need(key,value)
    ON op.key = need.key AND op.value = need.value
 GROUP BY op.object_id
HAVING count(*) = n;          -- has every required pair
```

`n = 0` (empty effective set) short-circuits to "all objects". This is the
single hot query of the system; `object_props_kv` makes the join an index
scan. The daemon may cache effective sets per directory, invalidated by
NOTIFY (§5.5).

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

Resolution then classifies the canonical path:

- **`lookup(V, path)` / `getattr`:** if `(V, path)` is a row in `view_dirs`
  it is a **directory** (return the row's mode/ctimes). Otherwise split into
  `parent = dirname(path)`, `base = basename(path)`; `parent` must be a
  directory (else `ENOENT`), and among `parent`'s computed members find the
  object whose `name` matches `base` (or the `name~<idprefix>` form, D2).
  Exactly one → **file**; zero → `ENOENT`.
- **`readdir(V, D)`:** synthetic `.`/`..`; child **directories** via
  `SELECT name FROM view_dirs WHERE view_name=$1 AND parent_path=$2`; then
  **files** via the membership query (§3.4) listed by `name`, applying D2
  disambiguation when two listed objects share a name. A file whose name
  collides with a child directory's name is shown with the D2 suffix on the
  *file*, since the directory owns the bare name.

---

## 5. Daemon internals

### 5.1 libpq connection model

A `PGconn` is not safe to share across threads. The **CLI** is
single-threaded and uses one `PGconn` for the lifetime of the process
(opened via `vfs_store_open`, closed by `vfs_store_close`).

The **FUSE daemon** allocates one `PGconn` per FUSE worker thread, lazily,
kept in thread-local storage. With libfuse3's default thread pool that's
roughly 10 connections — well within a typical local Postgres
`max_connections`.

Every `PGconn` runs `SET search_path TO "<schema>";` once at open time
(the schema is read from `config.toml`). The schema name is validated
against `^[A-Za-z_][A-Za-z0-9_]{0,62}$` before being interpolated.

### 5.2 Hot-path queries

The hot-path queries are:

- directory lookup in `view_dirs` (getattr, readdir of subdirs),
- the membership query of §3.4 (readdir of files, lookup of a file by name),
- object fetch by id (stat fillers, open),
- the mutation statements for `mkdir`/`rmdir` (`view_dirs`),
  `create`/`unlink`/`rename` (`objects` + `object_props`), and the
  size/mtime touch on write-close.

The daemon issues these inline via `PQexecParams` with binary parameters;
promoting them to `PQprepare`d statements per connection is a future
optimization once profiling shows parse/plan cost matters.

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

PostgreSQL `LISTEN/NOTIFY` solves the daemon-doesn't-see-CLI-edits problem:

- Every membership-changing mutation (CLI or daemon) issues `NOTIFY
  viewfs_change` with payload `<view_name>\t<parent_path>` (TAB-separated;
  empty parent means root) after its transaction commits.
- The daemon dedicates one background thread to `LISTEN viewfs_change` on
  its own `PGconn`. On a payload matching its view it calls
  `fuse_invalidate_path()` for the affected directory, dropping the kernel's
  dentry/attr cache for that directory's children.

Because membership is global to a view, a **property change** has no single
"parent" directory — a CLI `prop set`/`unset` that alters which directories
an object matches emits a view-wide invalidation (the listener drops the
whole tree). This keeps a 1–2 second FUSE attribute cache safe during
concurrent mutations.

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

`vfs check --fill-checksums` recomputes hash + state for any file
with `checksum IS NULL`. `vfs check --verify-checksums` re-hashes
each non-NULL row's content and reports mismatches.

### 5.7 Create flow

```
BEGIN;
  INSERT INTO objects (object_id, kind, name, size, mode, ...)
    VALUES (new_id, 'file', base, 0, mode, ...);
  -- assign effective(V, parent) as the new object's properties so it
  -- matches `parent` (the "files created in a directory take on its
  -- pairs" rule). flow is a directory attribute, not copied to the object.
  INSERT INTO object_props (object_id, key, value, ...) VALUES …;
COMMIT;
create_empty_content_file(new_id);                  -- after commit, fsync'd
open and return fh;
```

The parent directory must already exist (`mkdir` creates `view_dirs` rows;
`op_create` does not auto-make directories). A symlink (`op_symlink`,
`kind='symlink'`) is created the same way and likewise inherits the
directory's effective pairs. If post-commit content creation fails,
`vfs check` reports the object as content-missing.

### 5.8 Unlink / rmdir

- `unlink` (**D3**): delete the object row (cascading `object_props`) and
  its content blob. The file leaves every directory it matched. To de-list
  without deleting, change the object's properties instead.
- `rmdir`: refuse with `ENOTEMPTY` if the directory has any child directory
  **or** a non-empty computed membership; otherwise delete the `view_dirs`
  row (cascading its `view_dir_props`).

### 5.9 Rename within a view

- **File move:** the object's properties become
  `(props − effective(V, src_parent)) + effective(V, dst_parent)` and its
  `name` becomes the new basename; content is untouched. The file thereby
  leaves the source directory and appears in the destination.
- **Directory move:** re-parent the `view_dirs` subtree with a prefix-rewrite
  UPDATE, carrying its `view_dir_props`. Effective sets are always derived,
  so nothing else needs recomputing.
- Cross-view rename / `RENAME_EXCHANGE` return `EXDEV` / `EINVAL`.

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
name — and stashes it in `VIEWFS_STORE` for the rest of the process, so
subcommand parsers never need to know about it. The admin binary is `vfs`
and the daemon is `vfs-fuse`.

```
vfs init [STORE_PATH] [--pg CONNINFO] [--schema NAME] [--reinit]
vfs status

vfs view create NAME ["DESCRIPTION"]
vfs view list | show NAME | delete NAME

# Directory tree (a directory's filter is just properties on the directory).
vfs dir mkdir [VIEW] DIR
vfs dir rmdir [VIEW] DIR
vfs dir ls    [[VIEW] DIR]                    # computed contents

# Properties — one command for a file's props OR a directory's filter; the
# TARGET decides which. Targets come last (like chmod) and a list is allowed
# (omitted = the current directory inside a mount). A property is one
# KEY=VALUE token; for unset the value is optional (bare KEY = every value).
vfs prop set    KEY=VALUE   [TARGET...] [--flow]
vfs prop unset  KEY[=VALUE] [TARGET...]
vfs prop list   [TARGET...] [--effective]
vfs find --prop KEY[=VALUE] [--prop KEY[=VALUE]]...   # AND across pairs

vfs object import HOST_PATH [--name NAME] [--into VIEW:DIR]...
vfs object show ID|PREFIX
vfs object name ID|PREFIX [NEWNAME]           # get/set the intrinsic name
vfs object copy ID|PREFIX VIEW:DIR            # D4 lossless copy
vfs object id   VIEW VIEW_PATH
vfs object list [--orphaned]
vfs object delete ID|PREFIX | --orphaned [--dry-run]

vfs mount NAME [--ro] [--foreground] [--verbose] MOUNTPOINT
vfs mount                                     # no args: list current mounts
vfs umount MOUNTPOINT                         # wraps fusermount3 -u

vfs check [--fix] [--fill-checksums] [--verify-checksums] [--verbose]
```

`README.md` is the authoritative reference for the exact flags; the notable
points:

- The old explicit-mapping commands are **gone**: `view add`/`remove`/
  `populate`, `attr *`, and `tag *`. A "tag" is now just `prop set tag=…`.
- `TARGET` is an object id/prefix, a `VIEW:DIR`, or — inside a mount — a
  path/name resolved against the cwd. `prop` decides file-vs-directory from
  what the target names. `--flow`/`--effective` apply only to directories.
- `object import --into VIEW:DIR` assigns `effective(V,DIR)`'s pairs to the
  imported object (so it appears in `DIR`); `--name` overrides the basename.
  Content is copied into `objects/<aa>/<id>` via `tmp/` + atomic `rename`; a
  metadata-insert failure after the write unlinks the content to avoid an
  orphan.
- Prefix matching on object IDs uses PK range queries
  (`object_id >= 'a1f' AND object_id < 'a1g'`); an ambiguous prefix returns
  `VFS_ERR_AMBIGUOUS`.
- `init` refuses to overwrite an existing `config.toml` without `--reinit`;
  `--pg` is optional (falls back to `VIEWFS_PG_USER`/`VIEWFS_PG_DATABASE`
  then libpq's `PGHOST`/`PGPORT`/`PGUSER` env vars).

---

## 7. View membership model

Per spec §8.3 the README declares the model. **ViewFS uses dynamic,
property-driven membership:** a directory's contents are computed live by
the superset query of §3.4 against the directory's effective filter — no
stored placements, no batch materialization step. A directory *is* a saved
query; this is a strict generalization of attribute-based views.

### 7.1 Risks and trade-offs

| Risk | Mitigation |
|---|---|
| Membership query cost on large stores (relational division per readdir) | `object_props_kv` index + `HAVING count(*)=n`; daemon may cache effective sets per directory, invalidated by NOTIFY. |
| An empty-filter directory dumps the whole object table into one `ls` | Documented and intentional (the root behaves this way); `vfs dir ls` warns when `n=0`. |
| Name collisions among many matching objects | D2 id-suffixing; `vfs object name` to rename; collisions logged under `--verbose`. |
| Through-the-mount `cp` cannot retain the source's extra props | D4: `vfs object copy` is the lossless path; the mount `cp` limitation is documented. |
| `mv`/property edits making a file (dis)appear from unrelated directories | Inherent to a global-by-property model; the set that changes is exactly the objects matching the edited pairs. |

---

## 8. Security boundaries

What the daemon enforces:

- Canonicalizer rejects any path resolving above `/`, killing `..` escape
  (spec §11.1.1, test #20).
- Guessing backing-store paths through the mount is impossible: the daemon
  never opens by path string; it opens `objects/<aa>/<object_id>` and
  `object_id` is only obtainable via SQL (spec §11.1.5).
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
- `chmod` — updates `objects.mode` for a file or `view_dirs.mode` for a
  directory.
- `chown` — no-op on uid/gid but doesn't error.
- `access` — mode-bit check against the invoking user.
- `readlink`, `symlink` — implemented, with target canonicalization.
- `listxattr`, `getxattr`, `setxattr`, `removexattr` — mapped onto
  `object_props` behind a `user.viewfs.` prefix; `user.viewfs.<key>`
  round-trips as a `(key,value)` property. A single-valued `getxattr`
  returns the lexically-first value; `setxattr` replaces all values of the
  key.

### 9.3 Error mapping

Per spec §12.4. The membership model adds two documented behaviors: `rm`
deletes the object (D3) rather than just de-listing it, and a name shown
with a `~<idprefix>` suffix (D2) resolves back to its object.

---

## 10. Logging and diagnostics

- `vfs-fuse --verbose` enables per-op trace logs to stderr or
  `--log-file PATH`.
- All path-resolution rejections log the input path and the rule that
  fired.
- `vfs check` runs three phases:
  1. **DB integrity** — propertyless objects and objects matching no
     directory (informational orphans), directory-tree sanity (every
     `view_dirs` row reachable from its root; consistent `name`/
     `parent_path`), and orphan `object_props` rows.
  2. **Content↔object** — every file object has a content file of the
     expected size; every content file under `objects/<aa>/` has an object
     row (symlink objects are skipped).
  3. **Migrations** — `schema_migrations.version` matches the binary's
     expected version (1).
  `--fix` removes only unambiguous garbage (orphan content files). It never
  deletes an object row or anything that could be user data.

---

## 11. Testing

Two tiers:

1. **Unit tests** (libviewfs, exercised by a small C test harness):
   canonicalizer rules, schema migrations, property CRUD, the `effective()`
   walk (independent vs. flowed), superset membership matching, orphan
   detection, and prefix resolution.
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

```
ViewFS/
  README.md  Design.md  Rationale.md  INSTALL.md  RUNNING.md
  viewfs_fuse_prototype_spec.md
  Makefile
  src/
    libviewfs/                       # static library, links into both bins
      canonicalize.c                 # vfs_path_canonicalize
      config.c                       # minimal TOML-subset reader/writer
      object_id.c                    # 16-byte getrandom() id, lowercase hex
      object_store.c                 # content blob I/O (tmp+rename)
      store.c                        # store lifecycle + migrations + helpers
      views.c                        # views CRUD (root dir bootstrap)
      objects.c                      # objects CRUD + name + prefix resolve
      object_props.c                 # multi-valued object property CRUD
      view_dirs.c                    # directory tree + dir-prop filters + flow + effective set
      members.c                      # computed membership (superset match)
      find.c                         # find by property pairs
      checksum.c                     # SHA-256 stream helpers
      version.c                      # viewfs_version_string, vfs_error_str
      internal.h                     # private declarations
      migrations/
        0001_init.sql                # canonical schema (also embedded in
                                     # store.c — keep them in sync)
    fuse/                            # ./vfs-fuse
      main.c                         # argv, store/view validation, fuse_main
      ops.{c,h}                      # FUSE callbacks (membership) + viewfs_ctx
      conn_pool.{c,h}                # per-thread PGconn TLS
      notify.{c,h}                   # LISTEN thread + fuse_invalidate_path
    cli/                             # ./vfs
      main.c                         # top-level dispatcher + --store handling
      common.{c,h}                   # cli_open_store, cli_take_flag, etc.
      cmd_init.c     cmd_status.c
      cmd_view.c     cmd_dir.c
      cmd_object.c   cmd_prop.c
      cmd_find.c     cmd_check.c
      cmd_mount.c    cmd_mounts.c    # mount / list mounts
      cmd_unmount.c                  # `vfs umount` -> fusermount3 -u
  include/viewfs/
    viewfs.h                         # the only public header
  tests/
    unit/
      test_canonicalize.c
      test_object_id.c
      test_props.c                   # DB-backed property-model tests
    integration/
      lib.sh                         # shared per-test isolation + helpers
      run.sh                         # iterates test_*.sh
      test_*.sh                      # property-model end-to-end scripts
  examples/
    demo.sh                          # property-model demonstration
```

## 15. Open work

- Decide whether `vfs check --fix` should attempt to repair partial inserts
  (rare; needs a torn-write reproducer first).
- Promote the hot-path FUSE queries from inline `PQexecParams` to prepared
  statements if profiling shows parse/plan cost matters (§5.2).
- Cache directory effective sets in the daemon, invalidated by NOTIFY, if
  the per-`readdir` membership query proves costly on large stores.
- The CLI runs as the invoking OS user but connects to Postgres with
  whatever role the conninfo specifies. On Fedora with `trust` auth this is
  painless; stricter deployments need a dedicated PG role (see `INSTALL.md`).
