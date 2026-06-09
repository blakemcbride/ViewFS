# ViewFS

A Linux FUSE 3 prototype of a **view-based filesystem**: instead of a single
fixed hierarchy, files are organized into named, task-specific *views*.
Membership is **property-driven** — each directory carries a set of
property/value pairs, and a file appears in a directory exactly when the
file's properties are a **superset** of that directory's effective set.
Nothing records where a file "lives"; a directory's contents are *computed*
from properties. The same underlying object therefore surfaces in every
directory (and every view) whose filter it satisfies, edited in one place.

**Source:** <https://github.com/blakemcbride/ViewFS>

> **Experimental.** ViewFS is a research prototype. The on-disk format is
> not stable. The durability contract is: writes whose `close(2)`
> returned 0 are durable across power loss — the daemon `fsync`s the
> content file and updates the DB inside `op_flush`, which is what
> `close(2)` synchronizes against. In-flight (unclosed) writes are not
> durable. See "Durability" below for the full picture.

## What this prototype demonstrates

- Multiple named views over a shared object pool.
- **Property-driven membership:** directories carry `(key, value)` filter
  pairs; a file appears wherever its properties are a superset of the
  directory's effective filter. One object surfaces in many directories and
  views at once, with no copies and no per-file placement records.
- **Per-property "flow":** a directory pair can be marked to cascade to
  descendant directories, making a child a strict subset of its parent
  (cumulative); unmarked pairs constrain only their own directory.
- Multi-valued properties (a key may hold several values).
- Directories and their filters persist across unmount and remount.
- Files mounted under a view are accessed via ordinary POSIX calls
  (`open`, `read`, `write`, `readdir`, `mkdir`, `rename`, `symlink`, …).
  Creating a file inside a directory gives it that directory's properties;
  moving a file swaps the source directory's pairs for the destination's.
- `find --prop KEY[=VALUE]` queries; object properties round-trip through
  FUSE `setxattr`/`getxattr` under the `user.viewfs.` namespace.
- PostgreSQL `LISTEN/NOTIFY` driven cache invalidation so changes made
  through the CLI are visible inside running mounts within milliseconds.
- A self-diagnosing `viewfs check` that detects DB↔filesystem divergence.

## What this prototype does not attempt

- A kernel filesystem module or a new block-device filesystem.
- A production-ready on-disk format, quotas, snapshots, dedup, compression,
  or encryption.
- Process-level isolation beyond what FUSE itself provides — combine with
  mount namespaces / bubblewrap / containers for that.
- Full Linux DAC/MAC replacement; SELinux/AppArmor integration.
- Network filesystem or distributed-consistency features.
- Full crash-consistency for in-flight (unclosed) writes — only
  `close(2)`-completed writes are durable. See "Durability" below.

See:

- [`Rationale.md`](Rationale.md) — why this filesystem exists: the case for
  property-driven views over the place-it-by-hand directory tree.
- `INSTALL.md` — build, configure PostgreSQL, optional system-wide install.
- `RUNNING.md` — guided tutorial: views, directories, property filters,
  mounts, sharing, xattrs, diagnostics.
- `Plan-rewrite.md` — the property-driven model and the rewrite plan
  (authoritative for the current behavior).
- `Design.md` / `Plan.md` — the original explicit-mapping design and build
  plan (historical; superseded by the property model).
- `viewfs_fuse_prototype_spec.md` — the spec this prototype targets.

## Status

The prototype was first built explicit-mapping (one stored row per file
placement) per `Plan.md`, then **rewritten to the property-driven model**
described here per `Plan-rewrite.md`:

| Phase | Scope | Status |
|-------|-------|--------|
| R0 | Property schema + libviewfs (membership, effective sets, flow) | ✓ |
| R1 | CLI (`dir`, `prop`, `find --prop`, `object copy/name`) | ✓ |
| R2 | FUSE daemon over computed membership (read + write) | ✓ |
| R4 | Integration suite + docs/demo rewrite | ✓ |
| R5 | `check` invariants, name-collision disambiguation (D2) | ✓ |

`Design.md` and `Plan.md` document the original explicit-mapping build; the
authoritative description of the current model is `Plan-rewrite.md`.

## Architecture

```
┌──────────────────┐        ┌──────────────────┐
│   viewfs CLI     │        │  viewfs-fuse     │   one daemon per
│  (admin tool)    │        │  (FUSE daemon)   │   mounted view
└────────┬─────────┘        └────────┬─────────┘
         │                           │
         │   libviewfs (C, libpq + libfuse3)
         ▼                           ▼
   ┌─────────────────────────┐  ┌────────────────────────┐
   │   PostgreSQL (libpq)    │  │  $STORE on local disk  │
   │   schema = viewfs       │  │  config + content blobs│
   └─────────────────────────┘  └────────────────────────┘
```

Implementation language: **C11**. Metadata lives in PostgreSQL via libpq;
content blobs live on the host filesystem under `$STORE/objects/` named by
object id and sharded one level by the first two hex chars.

## Building

Fedora 44+ (the development target):

```sh
sudo dnf install fuse3 fuse3-devel libpq libpq-devel \
                 gcc make pkgconf-pkg-config postgresql
make
```

The `fuse3` package provides the `fusermount3` setuid helper used by
`viewfs unmount`; `fuse3-devel` provides `libfuse3` for the daemon link.
`postgresql` is needed for the client-side `psql` used by the demo
script and by the integration tests; the server itself can be installed
separately (`postgresql-server`).

The build produces two binaries in the project root:

- `./viewfs` — the admin CLI
- `./viewfs-fuse` — the FUSE daemon (invoked by `viewfs mount`)

`make clean` removes them; `make test` runs the unit tests
(see [Tests](#tests)).

## PostgreSQL setup

You need a running local PostgreSQL with a role ViewFS can connect as. You do
**not** need to create the database yourself: `viewfs init` creates it
automatically if it is missing (it connects to the `postgres` maintenance
database and runs `CREATE DATABASE`), as long as the role has the `CREATEDB`
privilege. The development setup uses the superuser `postgres` role:

```sh
sudo systemctl start postgresql
# pg_hba.conf trust or peer auth on the local socket suffices for dev.
# `viewfs init` will create the `viewfs` database on first run.
```

For a single-user setup that connects as yourself instead, give your role
`CREATEDB` (or superuser) so `init` can create the database:

```sh
sudo -u postgres createuser -s "$USER"   # -s = superuser; or use --createdb
```

## Quick start

```sh
export VIEWFS_PG_USER=postgres
./viewfs init /tmp/vfs
export VIEWFS_STORE=/tmp/vfs   # avoids repeating --store on every call
```

`viewfs init` builds its libpq conninfo from `VIEWFS_PG_USER` (optional)
and `VIEWFS_PG_DATABASE` (defaults to `viewfs` if unset); pass
`--pg CONNINFO` instead for non-default host, port, password, or other
libpq options.

`viewfs init` creates `/tmp/vfs/{config.toml,objects/,tmp/,daemons/,logs/}`
and creates the PostgreSQL `viewfs` schema with the tables defined in
`src/libviewfs/migrations/0001_init.sql`. Pass `--schema NAME` to use a
different schema name; pass `--reinit` to overwrite an existing
`config.toml`.

### Creating views and directories

A view is a named directory tree; each directory carries a property filter.

```sh
./viewfs view create docs 'document library'
./viewfs view list
./viewfs view show docs                 # prints the directory tree

./viewfs dir mkdir docs /by-author
./viewfs dir mkdir docs /by-author/blake
./viewfs dir mkdir docs /reports/2024   # auto-creates /reports
./viewfs view delete docs               # removes the view and its directories
```

A freshly made directory has an **empty** filter — and an empty filter is a
subset of everything, so an empty-filter directory (including every view's
root `/`) lists **every** object until you attach pairs to it.

### Attaching property filters to directories

A directory's filter is just properties on the directory, so the same
`prop` command manages it — the **target** says whether you mean a file or a
directory. A directory target is written `VIEW:DIR` (or a path/cwd inside a
mount; see below).

```sh
# /by-author/blake matches objects whose properties include author=blake
./viewfs prop set docs:/by-author/blake author blake

# --flow makes a pair cascade to descendant directories
./viewfs prop set docs:/reports kind report --flow
./viewfs prop set docs:/reports/2024 year 2024

./viewfs prop list docs:/reports/2024              # own pairs only
./viewfs prop list docs:/reports/2024 --effective  # incl. flowed ancestors
./viewfs prop unset docs:/reports/2024 year 2024
```

With the pairs above, `/reports/2024`'s *effective* filter is
`{kind=report, year=2024}` (its own `year` plus the flowed `kind`), so it is
a strict subset of `/reports`.

Inside a mounted view you rarely need to type `VIEW:DIR` at all — `prop` (and
`dir mkdir`/`rmdir`/`ls`) infer the target from where you're standing:

- a **directory** target can be omitted (acts on your current directory),
  given as `.`, or given as a relative/absolute path;
- a **file** target is just its name (or a path) in the current directory;
- `prop` decides file-vs-directory automatically from what the path names.

```sh
cd /tmp/mnt/docs/reports
viewfs dir mkdir 2024              # -> docs:/reports/2024 (relative path)
viewfs prop set . kind report --flow   # set on the current directory
cd 2024
viewfs prop set . year 2024       # filter on the dir you're in
viewfs prop list --effective      # this directory's effective filter
viewfs prop set report.txt reviewed yes   # a file's property, by name
viewfs prop list report.txt       # that file's properties
viewfs dir ls                     # the current dir's computed contents
```

(`--flow` and `--effective` apply only to directory targets; using them on a
file is an error. For `prop set`/`prop unset` the `TARGET` is required — use
`.` for the current directory — while `prop list`'s target may be omitted.)

### Importing files and giving them properties

```sh
echo 'Q3 revenue' > /tmp/q3.txt
# import and assign /reports' effective pairs in one step:
./viewfs object import /tmp/q3.txt --into docs:/reports
./viewfs object list
./viewfs object show <id|prefix>
./viewfs object name <id|prefix> q3.txt     # get/set the display name

# properties drive membership; set them directly too (multi-valued).
# the TARGET here is an object id, but it can also be a filename in a mount:
./viewfs prop set   <id> author blake
./viewfs prop set   <id> author jane        # same key, second value
./viewfs prop list  <id>
./viewfs prop unset <id> author jane        # drop one value
./viewfs find --prop kind=report --prop year=2024   # AND across pairs

# see a directory's computed contents without mounting:
./viewfs dir ls docs /reports/2024
```

`object import` copies content into `$STORE/objects/<aa>/<id>` via `tmp/` +
atomic `rename` and inserts the object row; `--into VIEW:DIR` then assigns
that directory's effective pairs so the object appears there. Object IDs are
32 hex characters; the CLI accepts any unambiguous prefix.

`viewfs object copy <id> VIEW:DIR` duplicates an object's content into a new
object that keeps the source's properties and additionally gains the
destination directory's pairs.

### Mounting and unmounting

```sh
mkdir -p /tmp/mnt/docs
./viewfs mount   docs /tmp/mnt/docs
./viewfs mounts                       # list every mounted view + its mountpoint
./viewfs unmount /tmp/mnt/docs
```

`viewfs mounts` reads `/proc/mounts` and lists each mounted ViewFS view, its
mode (`rw`/`ro`), and its mountpoint — for example:

```text
VIEW                 MODE   MOUNTPOINT
docs                 rw     /tmp/mnt/docs
archive              ro     /tmp/mnt/archive
```

It needs no `--store`/`$VIEWFS_STORE` and reports every ViewFS mount on the
system. `viewfs unmount` is a thin wrapper around `fusermount3 -u`; the
`fusermount3` setuid helper lives in the `fuse3` package on Fedora.

Mount options:

- `--ro` — read-only mount; writes return `EROFS`.
- `--foreground`, `-f` — keep the daemon attached to the terminal.
- `--verbose`, `-v` — per-callback tracing on stderr.

The daemon writes its PID file at `$STORE/daemons/<view>.pid` on mount
and removes it on unmount.

### Reading, writing, and renaming

```sh
cat /tmp/mnt/docs/reports/2024/q3.txt        # read
echo 'idea' > /tmp/mnt/docs/by-author/blake/idea.txt  # create: gains author=blake
mkdir /tmp/mnt/docs/sub                       # mkdir (empty filter)
mv /tmp/mnt/docs/by-author/blake/idea.txt /tmp/mnt/docs/reports/idea.txt  # move
rm /tmp/mnt/docs/reports/idea.txt             # unlink -> deletes the object
rmdir /tmp/mnt/docs/sub                       # rmdir (errors if it matches files)
```

Creating a file inside a directory automatically gives the new object that
directory's **effective** property pairs, so it immediately appears there.
A **move** (`mv`) keeps the same object but swaps the source directory's
pairs for the destination's (retaining any other properties); a **copy**
through the mount (`cp`) makes a brand-new object that takes on the
destination directory's pairs. **`rm` deletes the object outright** — to
remove a file from one directory without destroying it, change its
properties instead (`viewfs prop unset`, or `mv` it elsewhere).

> Two behaviors that follow directly from the model: an object appears under
> a view's root `/` (and any other empty-filter directory) because the empty
> filter matches everything; and a file in a child directory also appears in
> its parent whenever the parent's pair is flowed down, since the child is
> then a strict subset of the parent. Both are intentional.
>
> When two distinct objects share a display name in the same directory, the
> mount lists each as `name~<object-id-prefix>` (e.g. `report.txt~a1f8`) so
> they remain individually openable; a uniquely-named file is shown bare.

### How shared objects behave

Membership is a pure function of an object's properties and each directory's
filter, so one object surfaces in every directory and view it matches — no
copies. Writing through any mount updates that one underlying object:

```sh
./viewfs view create archive
./viewfs dir mkdir archive /2024
./viewfs prop set archive:/2024 year 2024
# an object with year=2024 now shows in BOTH docs:/reports/2024 and
# archive:/2024 — same object, two views, two filters.
```

Cross-mount changes are propagated to running daemons via PostgreSQL
`LISTEN/NOTIFY`. Each daemon holds a 2-second FUSE attr/entry cache as a
safety net; in practice CLI mutations land in `ls` output within tens of
milliseconds without re-mount.

### Properties (and xattrs)

Object properties are multi-valued `(key, value)` pairs and are the sole
driver of membership. A "tag" is just a property whose key is `tag`. The same
`viewfs prop` command manages properties on a **file** or the filter on a
**directory** — the `TARGET` (an object id, a `VIEW:DIR`, or a path/name in a
mount) decides which. Properties also round-trip through FUSE extended
attributes under the `user.viewfs.` namespace (everything else returns
`ENOTSUP`):

```sh
./viewfs prop set  <id> language C        # by object id
./viewfs prop list <id>
./viewfs find --prop language=C

cd /tmp/mnt/docs/reports
viewfs prop set  q3.txt language C         # the same file, by name in a mount
viewfs prop list q3.txt

setfattr -n user.viewfs.author -v 'blake' /tmp/mnt/docs/reports/q3.txt
getfattr -d /tmp/mnt/docs/reports/q3.txt
setfattr -x user.viewfs.author /tmp/mnt/docs/reports/q3.txt
```

Through FUSE an xattr is single-valued: `setxattr` replaces every value of
the key. Use `viewfs prop set` to add multiple values for one key.

### Symbolic links

```sh
ln -s ../q3.txt /tmp/mnt/docs/reports/q3-link
ls -l /tmp/mnt/docs/reports/q3-link
readlink /tmp/mnt/docs/reports/q3-link
```

Symlinks are stored as `kind='symlink'` objects with the target string
in `objects.symlink_target`. Resolution happens at the kernel level, so
an absolute target like `/etc/passwd` escapes the view at resolution
time — see [Security limitations](#security-limitations).

### Diagnostics

```sh
./viewfs status                # store path, schema, counts, content size
./viewfs check                 # three-phase consistency scan
./viewfs check --verbose       # list every offending row/file
./viewfs check --fix           # remove orphan content files (only)
```

`viewfs check` runs four phases:

1. **DB integrity** — objects with no properties (informational; they only
   match empty-filter directories), directory rows whose `parent_path`
   names a missing directory, directory rows whose `name`/`parent_path` are
   inconsistent with `dir_path`, and `object_props` rows referencing a
   missing object.
2. **Content ↔ object cross-check** — every object has a content file of
   the recorded size; every content file under `objects/` has a matching
   object row.
3. **Schema version** — `schema_migrations.version` matches the binary's
   expectation.
4. **Checksum coverage** — counts file objects with/without a stored
   checksum; `--fill-checksums` / `--verify-checksums` populate or verify.

`--fix` removes only unambiguous garbage (orphan content files with no DB
row). It never deletes objects or any file whose contents could be user
data.

Per-callback FUSE tracing is enabled with `viewfs mount --verbose`;
every callback logs its input path, intermediate state, and outcome to
stderr.

## Running the demonstration

`examples/demo.sh` walks through the property model end-to-end: initialize a
fresh store, build a view with a tree of property filters (including a flowed
pair), import files that gain those properties, show one object surfacing in
three directories at once, query with `find --prop`, mount and browse, create
a file inside a filter (watch it gain the directory's pairs), re-filter the
same objects through a second view, then unmount/remount and confirm
everything persists.

```sh
# Interactive (pauses between each step):
./examples/demo.sh

# Unattended (CI-style; no pauses):
./examples/demo.sh --unattended
```

The script is idempotent — it drops its own demo schema and wipes its
own scratch directories on each invocation, so it leaves the rest of
your store(s) untouched. Override the defaults via environment
variables:

| Variable | Default | Notes |
|---|---|---|
| `VIEWFS_DEMO_STORE`  | `/tmp/viewfs-demo`     | backing store directory |
| `VIEWFS_DEMO_MNT`    | `/tmp/viewfs-demo-mnt` | mountpoint root |
| `VIEWFS_DEMO_PG`     | `host=/var/run/postgresql user=postgres dbname=viewfs` | libpq conninfo |
| `VIEWFS_DEMO_SCHEMA` | `viewfs_demo`          | PG schema (dropped on each run) |

## Tests

```sh
make test          # runs both unit and integration suites
make unit-test     # just the C unit tests
make int-test      # just the integration suite
```

### Unit tests (`tests/unit/`)

- `test_canonicalize` — the path canonicalizer's rule set: leading slash
  required, `.` / `..` collapsed, escapes past root rejected, control
  characters rejected, duplicate slashes collapsed, deep paths.
- `test_object_id` — `vfs_object_id_generate` produces distinct 32-char
  lowercase-hex IDs; `vfs_object_id_valid` accepts those and rejects
  empty / short / 33-char / uppercase / non-hex / NULL inputs.
- `test_props` — (DB-backed) effective property sets (independent vs
  flowed), superset membership (multi-value AND, empty-set-matches-all),
  name resolution + ambiguity, orphan listing, rmdir rules, and the
  consistency-check helpers. Skips cleanly if no Postgres is reachable.

### Integration tests (`tests/integration/`)

Eight `test_*.sh` scripts exercise the property model end-to-end:

| File | Coverage |
|---|---|
| `test_init.sh`        | init + `status` + `check` on a fresh store |
| `test_dirs_props.sh`  | views, dirs, dir-prop filters (independent vs `--flow`), object props, `find --prop`, multi-value AND, `dir ls`, rmdir rules |
| `test_mount_read.sh`  | mounted `ls`/`cat`, empty-filter root, `--ro` enforcement |
| `test_mount_write.sh` | create (gains dir props), `cp`, `mkdir`, `mv` (prop swap), `rm` (object deleted), persistence across remount |
| `test_dupnames.sh`    | two objects sharing a name in one dir are shown + addressable as `name~<idprefix>` (D2) |
| `test_cross_view.sh`  | one object shared across two views by property match; isolation |
| `test_check.sh`       | `viewfs check` detects a deleted content file and exits non-zero |
| `test_crash.sh`       | `kill -9` the daemon after `close(2)`; `viewfs check` consistent; bytes survive remount |

Each test gets an isolated PostgreSQL schema (`viewfs_test_<pid>_<rand>`)
and a temp store directory, both dropped on exit. `tests/integration/lib.sh`
provides per-test setup, mount/unmount helpers that wait on
`/proc/mounts`, and assertion functions. `tests/integration/run.sh`
iterates the scripts and captures per-test logs under
`tests/integration/.logs/`.

Override the test Postgres connection via:

```sh
VIEWFS_TEST_PG='host=localhost user=postgres dbname=viewfs' make int-test
```

## Repository layout

```
ViewFS/
├── README.md                  -- this file
├── Design.md                  -- design decisions, phase-by-phase
├── Plan.md                    -- phased implementation plan
├── viewfs_fuse_prototype_spec.md
├── Makefile
├── include/viewfs/viewfs.h    -- public C API
├── src/
│   ├── libviewfs/             -- shared library (C, libpq)
│   │   ├── canonicalize.c     -- absolute-path canonicalization
│   │   ├── config.c           -- minimal TOML reader/writer
│   │   ├── object_id.c        -- 16-byte getrandom IDs
│   │   ├── object_store.c     -- content blob I/O (tmp+rename)
│   │   ├── store.c            -- store lifecycle + migrations
│   │   ├── views.c            -- views CRUD (root dir bootstrap)
│   │   ├── objects.c          -- objects CRUD + name + prefix resolve
│   │   ├── object_props.c     -- object property CRUD (multi-valued)
│   │   ├── view_dirs.c        -- directory tree + dir-prop filters + flow
│   │   ├── members.c          -- computed membership (superset match)
│   │   ├── find.c             -- find by property pairs
│   │   ├── notify.c           -- pg_notify emission helper
│   │   └── migrations/0001_init.sql
│   ├── cli/                   -- ./viewfs admin tool
│   │   ├── main.c             -- subcommand dispatcher
│   │   ├── common.{c,h}       -- store-open + flag helpers
│   │   ├── cmd_init.c        cmd_status.c
│   │   ├── cmd_view.c        cmd_dir.c
│   │   ├── cmd_object.c      cmd_prop.c
│   │   ├── cmd_find.c        cmd_check.c
│   │   ├── cmd_mount.c       cmd_unmount.c
│   └── fuse/                  -- ./viewfs-fuse daemon
│       ├── main.c             -- argv + fuse_main
│       ├── ops.{c,h}          -- FUSE callbacks (membership) + viewfs_ctx
│       ├── conn_pool.{c,h}    -- per-thread PGconn TLS
│       └── notify.{c,h}       -- LISTEN thread + fuse_invalidate_path
├── tests/
│   ├── unit/
│   │   ├── test_canonicalize.c
│   │   ├── test_object_id.c
│   │   └── test_props.c       -- DB-backed property-model tests
│   └── integration/
│       ├── lib.sh             -- shared helpers + per-test isolation
│       ├── run.sh             -- runner; iterates test_*.sh
│       └── test_*.sh          -- 6 property-model end-to-end scripts
├── Plan-rewrite.md            -- the property-model rewrite plan (authoritative)
└── examples/
    └── demo.sh                -- property-model demonstration
```

## Backing store layout

```
$STORE/
├── config.toml                -- store_version, shard_depth, pg conninfo, schema
├── objects/
│   └── a1/                    -- sharded by first 2 hex chars of object id
│       └── a1f8…              -- raw content, name == full object id
├── tmp/                       -- staging for atomic content writes
├── daemons/                   -- per-view PID files
└── logs/
```

PostgreSQL holds all metadata in a single configurable schema (default
`viewfs`): `objects` + multi-valued `object_props`, `views`, the per-view
`view_dirs` tree, and `view_dir_props` (the per-directory filters, each pair
carrying a `flow` flag). The canonical schema is
`src/libviewfs/migrations/0001_init.sql`; `Plan-rewrite.md` §3 explains it.

## Durability

The contract as of Phase 9:

- **Metadata** is persisted via PostgreSQL's WAL. With
  `synchronous_commit=on` (the default), every libviewfs and daemon
  transaction is durable at commit time.
- **Writes whose `close(2)` returned 0 are durable.** The daemon's
  `op_flush` (which the kernel invokes synchronously from `close(2)`)
  calls `fsync(fd)` and then updates `objects.size`/`mtime_ns`. If
  either step fails, `close(2)` returns an error and the DB is not
  updated, so the user knows the write may not have landed.
- **`object import` is durable** once the CLI returns success. The
  helper `fsync`s the tmp file, atomic-`rename`s into the shard
  directory, then `fsync`s the shard directory itself.
- **`op_create` inserts the object row and then creates its empty content
  file.** A crash in that narrow window can leave a row whose content file
  is missing; `viewfs check` reports it (`objects with missing content
  file`). On failure to create the content file the daemon deletes the row
  it just inserted.
- **In-flight (unclosed) writes are not durable.** If the daemon dies
  between `write(2)` returning and `close(2)`, the data sits in the
  kernel page cache and may be lost on power loss. Since `op_flush`
  hasn't run yet, the DB still reflects the pre-write size, so the
  state remains internally consistent (just at the older snapshot).
- **`viewfs check`** remains the recovery tool: it detects missing
  content files, size mismatches, and orphan content files, and
  `--fix` cleans up the unambiguous-garbage cases.

What this prototype still does NOT do:

- Force `O_SYNC` semantics on individual `write(2)` calls — only
  `close(2)` synchronizes.
- Two-phase commit between PostgreSQL and the host filesystem.
- Survive a disk that lies about `fsync` completion.

## Security limitations

ViewFS isolation applies only to accesses **through a mounted FUSE view**.
A process that can also access the backing store directly, or connect to
PostgreSQL with the same role, can see everything outside the view.

The daemon assumes its own user owns:

- the backing store directory tree, and
- the PostgreSQL role used to connect.

For real process isolation, combine ViewFS with one or more of:

- mount namespaces / `unshare`,
- containers,
- `chroot` where appropriate,
- bubblewrap or similar userspace sandboxing,
- SELinux/AppArmor (not integrated by ViewFS itself).

Symbolic-link targets are stored as opaque strings; their *resolution*
is performed by the kernel against the user's mount namespace, so an
absolute target like `/etc/passwd` will leak outside the view. Pair
ViewFS with a namespace if hard isolation matters.

See `viewfs_fuse_prototype_spec.md` §11 for the spec's full discussion
of security boundaries.

## Reporting issues

This is a research prototype. There is no support contract. Bug reports
are welcome via the usual channels; please include the output of
`viewfs status` and (if relevant) `viewfs check --verbose`.

## Credits

Conceived and orchestrated by **Blake McBride**.
The implementation was written by **Claude Code** (Anthropic), guided
phase-by-phase per `Plan.md` and `Design.md`.

## License

GNU General Public License, version 2. See [LICENSE](LICENSE) for the full text.
