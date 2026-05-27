# ViewFS Implementation Plan

Companion to `Design.md`. Phased plan for building the prototype.
Each phase is a single logical milestone that can be tested in isolation.
"Acceptance" lines reference the numbered criteria in
`viewfs_fuse_prototype_spec.md` §21 (AC1..AC16) and tests in §17 (T1..T20).

The phases are sequential where stated; testing work (Phase 8) overlaps
each preceding phase rather than waiting until the end.

---

## Phase 0 — Project skeleton

**Goal:** repo builds, runs `--help`, links against libfuse3 and libpq.

Deliverables:

1. Repository layout per `Design.md` §14.
2. `Makefile` with three targets: `libviewfs.a`, `viewfs` (CLI),
   `viewfs-fuse` (daemon). Uses `pkg-config --cflags --libs fuse3 libpq`.
   `make clean`, `make test` stubs.
3. `include/viewfs/viewfs.h` — public types (`vfs_store`, `vfs_object_id`,
   error enum), no implementations yet.
4. `src/cli/main.c` — subcommand dispatcher that prints usage and exits
   nonzero on unknown commands.
5. `src/fuse/main.c` — argv parsing stub that prints usage.
6. CI-style smoke: `make && ./viewfs --help && ./viewfs-fuse --help`
   succeeds.

Exit criterion: clean build on Fedora with no warnings at
`-Wall -Wextra -Wpedantic`.

---

## Phase 1 — libviewfs core (no FUSE)

**Goal:** create/inspect a backing store, talk to Postgres, manipulate
views/objects/mappings from the CLI.

Deliverables:

1. **Path canonicalizer** (`canonicalize.c`) with the rules in
   `Design.md` §4. Unit tests cover: trailing slash, `.`, `..`, escape past
   root, `\0`, empty input, relative input.
2. **Object ID generator** — 16 bytes from `getrandom(2)`, hex-encoded.
3. **Migration runner** (`store.c`) — applies `migrations/0001_init.sql`
   inside a transaction; idempotent; writes/reads `schema_migrations`.
4. **Connection helpers** — open a `PGconn` from `config.toml`, run
   `SET search_path`, prepare the statement set listed in `Design.md` §5.2.
5. **CRUD wrappers** for `objects`, `views`, `mappings`, `attributes`,
   `tags`. All take a `PGconn*` so they're usable from CLI single-shot or
   daemon worker thread.
6. **Object store I/O** (`object_store.c`) — write content via `tmp/` +
   `rename(2)`; read by opening `objects/<aa>/<id>`; sharded path helper.
7. **CLI commands** implemented and end-to-end testable:
   - `viewfs init STORE_PATH --pg CONNINFO`
   - `viewfs status`
   - `viewfs view create | list | show | delete`
   - `viewfs object import HOST_PATH`
   - `viewfs object show | paths | list [--orphaned]`
   - `viewfs view add | remove`
   - `viewfs attr set | get | remove`
   - `viewfs tag add | remove | list`
   - `viewfs find --tag | --attr`

Acceptance: AC1, AC2, AC3, AC4, AC14 (without FUSE).
Test coverage: T1, T2, T3, T4, T5, T6, T16, T17, T18, T19.

Exit criterion: a shell session can init a store, create two views, import
files, attach them at different paths in different views, and tag them —
all visible through `viewfs view show` and `viewfs object paths`.

---

## Phase 2 — FUSE read path

**Goal:** mount a view read-only; `ls` and `cat` work; file isolation
holds.

Deliverables:

1. `viewfs mount --view NAME MOUNTPOINT [--ro] [--foreground]` —
   `fork(2)` + `setsid(2)` for daemon mode; PID file in
   `$STORE/daemons/<view>.pid`.
2. `viewfs unmount MOUNTPOINT` — wraps `fusermount3 -u`.
3. **Per-thread PGconn pool** (`conn_pool.c`) — thread-local lazy init,
   prepared statements set up at first use, `SET search_path` on connect.
4. **FUSE callbacks (read-only set):** `getattr`, `readdir`, `open`,
   `read`, `release`, `statfs`, `access`.
5. Inode allocator: per-mount monotonic counter with refcount table
   driven by FUSE `lookup`/`forget`.
6. `--ro` honored: writes return `EROFS`.

Acceptance: AC5, AC6 (read side), AC7.
Test coverage: T7, T8, T9, T10, T11, T15 (read leg), T20.

Exit criterion: spec §18 demo steps 1–8 (init, create views, import,
attach, mount both, list contents) work end-to-end with read-only mounts.

---

## Phase 3 — FUSE write path

**Goal:** mounted views become read-write; spec demo steps 9–12 run.

Deliverables:

1. **FUSE callbacks (write set):** `create`, `write`, `truncate`,
   `unlink`, `mkdir`, `rmdir`, `rename`, `utimens`, `flush`, `fsync`,
   `chmod`, `chown` (no-op).
2. `ensure_parent_dirs` helper used by `create`, `mkdir`, and CLI
   `view add` — auto-creates explicit directory mappings.
3. `release` flushes `objects.size` + `objects.mtime_ns` in one short
   transaction per closed write fd.
4. `rename` handled in a single transaction inside one view; cross-mount
   rename returns `EXDEV` so userland falls back to copy+unlink.
5. Atomic content write: when `truncate(0)` precedes write (common with
   editors), the daemon still writes in place — atomicity is handled at
   the object level only on `object import`.

Acceptance: AC6 (full), AC10, AC11, AC12, AC13.
Test coverage: T12, T13, T14, T15.

Exit criterion: full spec §18 demonstration script runs unattended,
including the post-unmount/remount persistence step.

---

## Phase 4 — Cache invalidation via LISTEN/NOTIFY

**Goal:** CLI mutations are visible through a running mount within a
second, without disabling FUSE attribute caching.

Deliverables:

1. CLI emits `NOTIFY viewfs_change, '<view_name>'` inside every mutating
   command's transaction.
2. Daemon background thread (`notify.c`) runs `LISTEN viewfs_change` on a
   dedicated `PGconn`; on receipt, calls
   `fuse_lowlevel_notify_inval_entry()`/`_inval_inode()` for affected
   paths.
3. Daemon mount defaults to `-o entry_timeout=2,attr_timeout=2`; `--verbose`
   logs every invalidation.

Acceptance: improves AC10 behavior in concurrent use; doesn't break
existing tests.
Test coverage: new integration test — mount view A, run `viewfs view add`
from a second shell, verify the new file appears in `ls` within 2 s
without needing to remount.

Exit criterion: the new test passes; all prior tests still pass.

---

## Phase 5 — Optional FUSE callbacks (stretch within prototype)

**Goal:** broader POSIX compatibility for editors and shell tools.

Deliverables (in priority order, stop when time is tight):

1. `readlink` / `symlink` — store as `entry_kind='symlink'` mapping or as
   an object with `kind='symlink'` plus `symlink_target`. Document the
   choice in the README. Target canonicalization rejects escapes.
2. `listxattr`, `getxattr`, `setxattr`, `removexattr` — `user.viewfs.<key>`
   xattrs round-trip through the `attributes` table.
3. `viewfs view populate VIEW --tag TAG --under VIEW_PATH` — batch-insert
   mappings for all objects matching a tag.

Acceptance: extends AC14.
Test coverage: extend T16/T17/T18 with xattr round-trip; add a populate
test.

Exit criterion: README documents which optional ops shipped and which were
deferred.

---

## Phase 6 — `viewfs check` and diagnostics polish

**Goal:** the prototype can self-diagnose.

Deliverables:

1. `viewfs check` runs the three phases from `Design.md` §10.
2. `viewfs check --fix` removes unambiguous garbage only.
3. `viewfs-fuse --verbose` traces every callback with input path,
   canonicalized path, and outcome.
4. `viewfs status` prints store path, schema version, view count, object
   count, orphan count, and total content size.

Exit criterion: a manually corrupted store (e.g., a deleted content file)
is detected by `viewfs check` with a clear message; `--fix` does not make
the situation worse.

---

## Phase 7 — Demo script and README

**Goal:** anyone can clone, build, and see the prototype work.

Deliverables:

1. `examples/demo.sh` implementing spec §18 verbatim, with `read -r`
   pauses, annotated output, and a `--unattended` mode for CI.
2. `README.md` covering every bullet of spec §20:
   - what the prototype demonstrates and explicitly does not solve
   - Fedora build instructions
   - `viewfs init` walkthrough
   - view/object/mapping/attribute/tag examples
   - mount/unmount, including the `fusermount3` package note
   - security limitations (spec §11.2, §11.3)
   - how to run tests and the demo
   - explicit "experimental, not for irreplaceable data" warning

Acceptance: AC16.

Exit criterion: a fresh Fedora VM with `dnf install` line from
`Design.md` §13 can run `make && ./examples/demo.sh --unattended` to
green.

---

## Phase 8 — Tests (parallel track)

Tests are written alongside each phase, but tracked here as a single
deliverable so spec §17 coverage is auditable.

Deliverables:

1. **Unit tests** (`tests/unit/`): canonicalizer, ID generator, prefix
   resolution, schema migration idempotency, mapping CRUD.
2. **Integration harness** (`tests/integration/run.sh`): creates an
   ephemeral schema `viewfs_test_<pid>_<n>`, points the CLI/daemon at it
   via a temp `config.toml`, runs the test, tears down with
   `DROP SCHEMA ... CASCADE` and `fusermount3 -u`.
3. **Integration tests** mapping 1:1 to spec §17 tests T1–T20.
4. **`make test`** runs both tiers; nonzero exit on any failure.

Acceptance: AC15.

Exit criterion: every spec §17 test has a named test case that passes
without manual intervention.

---

## Phase 9 — Power-loss resilience

**Goal:** narrow the DB↔filesystem divergence window so an unexpected
power loss leaves a recoverable state in the common cases. This is
*not* full crash-consistency (that remains out of scope per spec §4.2);
the goal is to upgrade the prototype from "best effort, recover via
`viewfs check`" to "writes you've completed survive an immediate power
loss."

Background (state at end of Phase 5):

- Metadata: Postgres WAL + `synchronous_commit=on` already gives durable
  metadata at every transaction commit.
- Content: `op_write` is a pass-through `pwrite`, `op_release` does not
  `fsync`, and `op_create`'s empty content file is not synced. A crash
  between the kernel acknowledging a write and the page cache flush
  loses data. `vfs_content_import_host` `fsync`s the tmp file but
  ignores the result and never fsyncs the parent directory after the
  atomic `rename`.
- DB↔FS coordination: `op_release` updates `objects.size`/`mtime_ns` in
  a separate transaction from the host `write()`; a crash between the
  two leaves the DB stale.

Deliverables (in priority order):

1. **`fsync` on close for write fds.** `op_release` for a fd opened
   with write access does `fsync(fd)` (or `fdatasync`) before the DB
   UPDATE. This way, the DB's recorded size/mtime is never newer than
   what's on disk after a crash. Cost: one fsync per close-after-write.

2. **`fsync` the parent directory after content `rename`.** In
   `vfs_content_import_host`, after the atomic rename into
   `objects/<aa>/<id>`, open the shard directory and fsync it. Same for
   `vfs_content_create_empty`. Otherwise the rename may be lost across
   a crash even though the inode was atomically swapped.

3. **fsync errors are now fatal.** Audit every existing fsync; remove
   the "best effort; not fatal for prototype" comments. A failed fsync
   bubbles up as `-EIO` to the caller. (POSIX says reads on a fd whose
   fsync failed may not observe the unflushed bytes; the safe move is
   to error.)

4. **Pre-commit content creation for `op_create`.** Reorder so the
   empty content file is created and fsynced *before* the metadata
   transaction commits. On commit failure, unlink the content file. On
   content-create failure, abort with no DB change. This eliminates the
   "DB row without content" window.

5. **`viewfs check` smoke test on a simulated crash.** Add an
   integration test that:
   - Imports an object, writes through the FUSE mount, before
     `fsync`/release calls `kill -9` the daemon (or holds the mount
     while the journal is dropped).
   - Re-mounts and runs `viewfs check`.
   - Asserts the store is reported consistent (zero issue classes).

6. **README update.** Replace the "experimental, not for irreplaceable
   data" caveat with a more nuanced statement: writes that have
   returned from `close(2)` are durable; in-flight (unclosed) writes
   are not.

Out of scope for this phase (deferred to a future phase or never):

- Full two-phase commit between Postgres and the host filesystem (e.g.
  a journal table referencing `.pending-<txid>` content files).
- Moving content storage into Postgres `bytea` (loses pass-through
  performance).
- Crash-consistency guarantees during in-flight FUSE writes (would
  require an `O_SYNC`-equivalent or fsync-on-every-write mode).
- Replication / multi-host durability.

Acceptance: an `kill -9` of the daemon after a successful `close(2)` on
a FUSE write does not produce any `viewfs check` issue class on the
next mount.

Exit criterion: tests in §5 above pass; README documents the upgraded
durability contract; Design.md gains a Phase 9 implementation-notes
section.

---

## Phase order summary

```
Phase 0  Project skeleton
Phase 1  libviewfs + CLI (no FUSE)        ← AC1–AC4, AC14 partial
Phase 2  FUSE read path                   ← AC5–AC7
Phase 3  FUSE write path                  ← AC6 full, AC10–AC13
Phase 4  LISTEN/NOTIFY invalidation
Phase 5  Optional FUSE ops (stretch)
Phase 6  viewfs check + diagnostics
Phase 7  demo.sh + README                 ← AC16
Phase 8  Tests (alongside 1–7)            ← AC15
Phase 9  Power-loss resilience
```

Phases 0–3 are the minimum viable prototype that meets the spec's
acceptance criteria. Phases 4–7 raise polish to demonstration quality.
Phase 5 is explicitly stretch; document any deferred ops in the README.
Phase 9 was added after the initial plan to harden durability so that a
`close(2)` that returned 0 implies the bytes survive a power loss.

---

## Risks and mitigations

| Risk | Mitigation |
|------|------------|
| libpq + libfuse threading interaction subtle | One `PGconn` per FUSE worker thread, kept in TLS; never share. Validated by a stress test in Phase 2. |
| Editors using rename-temp-then-replace pattern hit edge cases | Phase 3 includes explicit tests for `vim`'s atomic save flow. |
| Postgres unavailable mid-mount | Daemon returns `EIO` but stays alive so `fusermount3 -u` works; README documents the constraint. |
| FUSE entry/attr caching masks CLI changes | Phase 4 LISTEN/NOTIFY closes the gap; Phase 2/3 can run with 0-second timeouts as a stopgap. |
| Per-test Postgres schema teardown leaks on crash | Harness `trap` runs cleanup; an offline `viewfs-test-gc` script drops any `viewfs_test_*` schemas older than 1h. |
