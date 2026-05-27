# ViewFS

A Linux FUSE 3 prototype of a **view-based filesystem**: instead of a single
fixed hierarchy, files are organized into named, task-specific *views*. The
same file can appear in multiple views, under different paths, with different
names — but it's the same underlying object, edited in one place.

**Source:** <https://github.com/blakemcbride/ViewFS>

> **Experimental.** ViewFS is a research prototype. The on-disk format is
> not stable. The durability contract is: writes whose `close(2)`
> returned 0 are durable across power loss — the daemon `fsync`s the
> content file and updates the DB inside `op_flush`, which is what
> `close(2)` synchronizes against. In-flight (unclosed) writes are not
> durable. See "Durability" below for the full picture.

## What this prototype demonstrates

- Multiple named views over a shared object pool.
- One file object → many view paths, possibly with different names.
- Per-view mappings persist across unmount and remount.
- Files mounted under a view are accessed via ordinary POSIX calls
  (`open`, `read`, `write`, `readdir`, `mkdir`, `rename`, `symlink`, …).
- Arbitrary attributes and tags on objects, with `find --tag` / `--attr`
  queries and round-trip support through FUSE `setxattr`/`getxattr`.
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

- `INSTALL.md` — build, configure PostgreSQL, optional system-wide install.
- `RUNNING.md` — guided tutorial: views, mounts, sharing, tags, xattrs, diagnostics.
- `Design.md` — implementation design, phase by phase.
- `viewfs_fuse_prototype_spec.md` — the spec this prototype targets.

## Status

Built incrementally per `Plan.md`:

| Phase | Scope | Status |
|-------|-------|--------|
| 0 | Project skeleton, build system | ✓ |
| 1 | libviewfs core + CLI (no FUSE) | ✓ |
| 2 | FUSE read path | ✓ |
| 3 | FUSE write path | ✓ |
| 4 | LISTEN/NOTIFY cache invalidation | ✓ |
| 5 | Optional FUSE ops (symlinks, xattrs, populate) | ✓ |
| 6 | `viewfs check`, diagnostics polish | ✓ |
| 7 | Demo script + full README | ✓ |
| 8 | Tests (alongside 1–7) | ✓ |
| 9 | Power-loss resilience (fsync, content-before-commit) | ✓ |

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

You need a running local PostgreSQL with a role and database for ViewFS.
The development setup uses the `postgres` role:

```sh
sudo systemctl start postgresql
sudo -u postgres createdb viewfs
# pg_hba.conf trust or peer auth on local socket suffices for dev
```

For a single-user setup that connects as yourself instead:

```sh
sudo -u postgres createuser -s "$USER"
sudo -u postgres createdb -O "$USER" viewfs
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

### Creating views

```sh
./viewfs view create programming --description 'source code'
./viewfs view create writing     --description 'docs and notes'
./viewfs view list
./viewfs view show programming
./viewfs view delete programming   # removes the view and all its mappings
```

### Importing files

```sh
echo 'int main(void) { return 0; }' > /tmp/foo.c
./viewfs object import /tmp/foo.c
./viewfs object list
./viewfs object show <id|prefix>
./viewfs object paths <id|prefix>     # every view path pointing at it
```

`object import` reads a host file, copies its content into
`$STORE/objects/<aa>/<id>` via `tmp/` + atomic `rename`, and inserts the
object row. Object IDs are 32 hex characters; the CLI accepts any
unambiguous prefix.

### Mapping objects into views

```sh
# Attach at any chosen view path; parent directories are auto-created.
./viewfs view add programming <obj> /src/foo.c
./viewfs view add writing     <obj> /docs/foo-notes.md

# Or attach at import time:
./viewfs object import /tmp/bar.c \
    --into programming:/src/bar.c \
    --into writing:/articles/bar-notes.md

./viewfs view remove programming /src/foo.c   # detach (object survives)
```

### Mounting and unmounting

```sh
mkdir -p /tmp/mnt/programming
./viewfs mount   programming /tmp/mnt/programming
./viewfs unmount /tmp/mnt/programming
```

`viewfs unmount` is a thin wrapper around `fusermount3 -u`. The
`fusermount3` setuid helper lives in the `fuse3` package on Fedora.

Mount options:

- `--ro` — read-only mount; writes return `EROFS`.
- `--foreground`, `-f` — keep the daemon attached to the terminal.
- `--verbose`, `-v` — per-callback tracing on stderr.

The daemon writes its PID file at `$STORE/daemons/<view>.pid` on mount
and removes it on unmount.

### Reading, writing, and renaming

```sh
cat /tmp/mnt/programming/src/foo.c          # read
echo 'new content' > /tmp/mnt/programming/new.txt   # create + write
mkdir /tmp/mnt/programming/sub               # mkdir
mv /tmp/mnt/programming/new.txt /tmp/mnt/programming/sub/moved.txt  # rename
rm /tmp/mnt/programming/sub/moved.txt        # unlink
rmdir /tmp/mnt/programming/sub               # rmdir (errors if non-empty)
```

### How shared objects behave

A single underlying object can be mapped into multiple views. Writing
through any one mount updates the shared file:

```sh
./viewfs view add programming <obj> /README.md
./viewfs view add writing     <obj> /docs/readme.md

echo 'OVERWRITTEN' > /tmp/mnt/programming/README.md
cat /tmp/mnt/writing/docs/readme.md     # -> OVERWRITTEN
```

Renaming a mapping affects only that view:

```sh
mv /tmp/mnt/programming/README.md /tmp/mnt/programming/README-renamed.md
./viewfs object paths <obj>
#   programming:/README-renamed.md
#   writing:/docs/readme.md          (unchanged)
```

Cross-mount changes are propagated to running daemons via PostgreSQL
`LISTEN/NOTIFY`. Each daemon holds a 2-second FUSE attr/entry cache as a
safety net; in practice CLI mutations land in `ls` output within tens of
milliseconds without re-mount.

### Tags and attributes

Tags and attributes are object-level metadata, queryable both from the
CLI and through FUSE extended attributes.

```sh
./viewfs tag add  <obj> project:compiler
./viewfs tag list <obj>
./viewfs find --tag project:compiler

./viewfs attr set    <obj> language C
./viewfs attr get    <obj>
./viewfs attr remove <obj> language
./viewfs find --attr language=C
```

The same `attributes` rows are exposed through FUSE under the
`user.viewfs.` xattr namespace (everything else returns `ENOTSUP`):

```sh
setfattr -n user.viewfs.author -v 'blake' /tmp/mnt/programming/README.md
getfattr -d /tmp/mnt/programming/README.md
setfattr -x user.viewfs.author /tmp/mnt/programming/README.md
```

A batch tag-to-view operation lets you materialize a view from a tag:

```sh
./viewfs view populate archive --tag project:compiler --under /by-tag/compiler
# adds archive:/by-tag/compiler/<basename> for every tagged object
```

### Symbolic links

```sh
ln -s /src/foo.c /tmp/mnt/programming/foo-link
ls -l /tmp/mnt/programming/foo-link    # lrwxrwxrwx ... -> /src/foo.c
readlink /tmp/mnt/programming/foo-link
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

`viewfs check` runs three phases:

1. **DB integrity** — orphan objects, mappings with dangling
   `object_id`, mappings violating the `dir`-invariant CHECK.
2. **Content ↔ object cross-check** — every object has a content file
   of the recorded size; every content file under `objects/` has a
   matching object row.
3. **Schema version** — `schema_migrations.version` matches the
   binary's expectation.

`--fix` removes only unambiguous garbage (orphan content files with no
DB row). It never deletes objects, mappings, or any file whose contents
could be user data.

Per-callback FUSE tracing is enabled with `viewfs mount --verbose`;
every callback logs its input path, intermediate state, and outcome to
stderr.

## Running the demonstration

`examples/demo.sh` walks through the spec §18 demonstration end-to-end:
initialize a fresh store, create two views, import three files, map one
shared file into both views under different paths, mount both, modify
through one view, verify visibility through the other, rename in one
view, unmount/remount, and confirm mappings persist.

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

### Integration tests (`tests/integration/`)

Fourteen `test_*.sh` scripts: thirteen cover all 20 numbered tests from
spec §17, plus a Phase-9 crash-recovery test.

| File | Coverage |
|---|---|
| `test_init.sh`        | T1  initializing a backing store |
| `test_views.sh`       | T2 + T4  view create, add object to one view |
| `test_import.sh`      | T3  import |
| `test_sharing.sh`     | T5 + T6  same object → two views, different paths |
| `test_mount_ls.sh`    | T7 + T8 + T9  mount, list visible, invisible hidden |
| `test_read.sh`        | T10 + T11  read visible / ENOENT for missing |
| `test_cross_view.sh`  | T12  write through one view, observed via another |
| `test_rename.sh`      | T13  rename in one view doesn't affect others |
| `test_unlink.sh`      | T14  unlink in one view doesn't remove object |
| `test_persist.sh`     | T15  mappings persist across unmount + remount |
| `test_attrs.sh`       | T16 + T19  attribute set/get + find by attribute |
| `test_tags.sh`        | T17 + T18  tag add/list + find by tag |
| `test_traversal.sh`   | T20  `..` traversal handled safely |
| `test_crash.sh`       | Phase 9: kill -9 the daemon after `close(2)`, run `viewfs check` |

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
│   │   ├── views.c            -- views CRUD
│   │   ├── objects.c          -- objects CRUD + prefix resolve
│   │   ├── mappings.c         -- mappings CRUD + ensure_parent_dirs
│   │   ├── attrs.c            -- attribute CRUD
│   │   ├── tags.c             -- tag CRUD
│   │   ├── find.c             -- joined queries for find --tag/--attr
│   │   ├── notify.c           -- pg_notify emission helper
│   │   └── migrations/0001_init.sql
│   ├── cli/                   -- ./viewfs admin tool
│   │   ├── main.c             -- subcommand dispatcher
│   │   ├── common.{c,h}       -- store-open + flag helpers
│   │   ├── cmd_init.c        cmd_status.c
│   │   ├── cmd_view.c        cmd_object.c
│   │   ├── cmd_attr.c        cmd_tag.c
│   │   ├── cmd_find.c        cmd_check.c
│   │   ├── cmd_mount.c       cmd_unmount.c
│   └── fuse/                  -- ./viewfs-fuse daemon
│       ├── main.c             -- argv + fuse_main
│       ├── ops.{c,h}          -- FUSE callbacks + viewfs_ctx
│       ├── conn_pool.{c,h}    -- per-thread PGconn TLS
│       └── notify.{c,h}       -- LISTEN thread + fuse_invalidate_path
├── tests/
│   ├── unit/
│   │   ├── test_canonicalize.c
│   │   └── test_object_id.c
│   └── integration/
│       ├── lib.sh             -- shared helpers + per-test isolation
│       ├── run.sh             -- runner; iterates test_*.sh
│       └── test_*.sh          -- 13 scripts mapping to spec §17 T1–T20
└── examples/
    └── demo.sh                -- spec §18 demonstration
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
`viewfs`). See `Design.md` §3 for the canonical schema; the migration
itself is `src/libviewfs/migrations/0001_init.sql`.

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
- **`op_create` creates the empty content file (and `fsync`s it and
  the shard directory) BEFORE the metadata transaction commits.** A
  crash between content creation and commit leaves an orphan content
  file — `viewfs check --fix` removes those. A crash after commit
  leaves a fully consistent new object.
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
