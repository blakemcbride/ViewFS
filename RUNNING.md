# A guided tour of ViewFS

This document walks you through ViewFS by example. Each section is
short, has commands you can paste, and shows the output you should
expect. By the end you'll have created a backing store, mounted a
view, populated it by copying files in, mounted a second view that
shares some of those files, queried metadata, and used the diagnostic
tools.

Before you start, make sure you've followed `INSTALL.md` so you have:

- `viewfs` and `viewfs-fuse` on your `$PATH` (or in the current
  directory),
- a running PostgreSQL with a database called `viewfs` (or whatever
  name you used),
- `VIEWFS_STORE` set to a path you don't mind clearing.

For this tutorial I'll use:

```sh
export VIEWFS_STORE=$HOME/tutorial-vfs
export VIEWFS_PG_USER=postgres
export VIEWFS_PG_DATABASE=viewfs
```

`viewfs init` reads `VIEWFS_PG_USER` and `VIEWFS_PG_DATABASE` when `--pg`
is not given; the host falls back to libpq's local Unix socket. If you
need a different host, port, password, or other libpq option, pass
`--pg CONNINFO` instead.

---

## 1. What's the mental model?

In ViewFS:

- **Objects** are file-shaped things with a stable 32-hex-character ID.
  Their content lives on disk; their metadata lives in PostgreSQL.
- **Views** are named hierarchies. Each view is a separate
  user-facing directory tree.
- **Mappings** connect a view-path to an object. The same object can
  appear in many views under different paths.
- Mounting a view exposes it at a mountpoint as if it were any other
  Linux filesystem.

The everyday workflow is: create a view, mount it, copy files into the
mountpoint with ordinary tools (`cp`, redirection, `mkdir`, …). Each
copied file becomes an object on disk and a mapping in that view. To
make a file *also* appear in a second view — the central feature — use
`viewfs view add` to attach the existing object at a chosen path in
the other view.

When you write through a mount, you change the object's content, so
*every* mapping that points at it sees the change. When you rename
inside a mount, only that view's mapping moves.

---

## 2. Initialize a backing store

```sh
viewfs init "$VIEWFS_STORE"
```

Expected output:

```
Initialized ViewFS store at /home/you/tutorial-vfs
```

What just happened:

- A directory tree was created at `$VIEWFS_STORE` with `config.toml`,
  `objects/`, `tmp/`, `daemons/`, and `logs/`.
- A PostgreSQL schema named `viewfs` was created in your database, and
  the v1 schema migration was applied to it.

Confirm everything looks healthy:

```sh
viewfs status
```

Expected output (paths and counts may differ):

```
ViewFS store: /home/you/tutorial-vfs
  schema:           viewfs
  schema_version:   1 (binary expects 1)
  conninfo:         user='postgres' dbname='viewfs'
  views:            0
  objects:          0 (orphans: 0)
  mappings:         0
  content storage:  0 file(s), 0 B (0 bytes)
```

---

## 3. Create a view

Views are the named hierarchies your files will appear in. Start with
one:

```sh
viewfs view create programming --description 'source code'
viewfs view list
```

Output:

```
Created view 'programming'
programming              source code
```

It's an empty named tree; no files are in it yet.

---

## 4. Mount the view

A mount exposes a view at a directory of your choice:

```sh
mkdir -p ~/mnt/programming
viewfs mount --view programming ~/mnt/programming
```

`viewfs mount` spawns the daemon, which backgrounds itself and starts
serving. Pass `--foreground` if you want the daemon attached to your
terminal (useful with `--verbose` for per-callback tracing, covered
later).

Confirm:

```sh
mount | grep viewfs
ls ~/mnt/programming         # empty
```

```
viewfs:programming on /home/you/mnt/programming type fuse (rw,...,default_permissions)
```

---

## 5. Populate the view by copying files in

The mounted view is a normal directory as far as your shell is
concerned. Drop files in with `cp`, redirection, `mkdir`, or any tool
you'd use against a regular filesystem:

```sh
echo '# README' > ~/mnt/programming/README.md

mkdir ~/mnt/programming/src
echo 'int main(void){return 0;}' > ~/mnt/programming/src/hello.c
```

To bulk-load a whole tree, `cp -r` works as you'd expect:

```sh
mkdir -p /tmp/sample-tree/utils
echo 'pi=3.14' > /tmp/sample-tree/utils/math.h
echo 'hi'     > /tmp/sample-tree/notes.txt

cp -r /tmp/sample-tree ~/mnt/programming/
```

Each new file under the mountpoint becomes one **object** on disk
(under `$VIEWFS_STORE/objects/<aa>/<id>`) and one **mapping** in the
`programming` view. Directories you create are implicit in the
mappings; you don't need to register them separately.

If you'd rather populate the store without mounting a view (useful for
scripts, or for dropping the same file into several views at once),
see §10 "Importing without a mount."

---

## 6. Browse and inspect

The mountpoint behaves like a normal Linux directory:

```sh
ls -lR ~/mnt/programming
cat ~/mnt/programming/src/hello.c
```

From the CLI side, look at what got created:

```sh
viewfs object list
viewfs view show programming
```

```
1db84d28...                  file              25  mode=0644  -
5fef537...                   file              10  mode=0644  -
92a01122...                  file               3  mode=0644  -
...
view 'programming':
  file   /README.md                               -> 5fef5374...
  dir    /sample-tree
  file   /sample-tree/notes.txt                   -> 92a01122...
  dir    /sample-tree/utils
  file   /sample-tree/utils/math.h                -> ...
  dir    /src
  file   /src/hello.c                             -> 1db84d28...
```

The trailing `-` in `object list` means "no source path recorded" —
expected for objects created through a mount. (Objects added via
`viewfs object import` carry the host path they came from.)

You'll refer to objects by ID or by any unambiguous *prefix* (e.g.
the first 8 hex characters). To look up the ID of a particular file
without scanning the whole view listing:

```sh
viewfs object id programming /src/hello.c
```

prints just the ID on stdout, so it composes cleanly with `$(...)`.

---

## 7. Sharing one file across views

The central feature of ViewFS is that one underlying file can appear
in multiple views under different paths. Create a second view and
attach one of the existing objects to it:

```sh
viewfs view create writing --description 'docs and notes'

# Grab the object ID of the README we created above:
README=$(viewfs object id programming /README.md)

# Attach the same object at a different path in the writing view:
viewfs view add writing "$README" /docs/readme.md
```

Mount the new view and you'll see the same file:

```sh
mkdir -p ~/mnt/writing
viewfs mount --view writing ~/mnt/writing
cat ~/mnt/writing/docs/readme.md
```

```
# README
```

Two view-paths in two views, but one object on disk:

```sh
viewfs object paths "$README"
```

```
object 5fef5374...:
  programming:/README.md
  writing:/docs/readme.md
```

Note that the `writing` mountpoint shows only the mappings in that
view — the rest of `programming`'s files (`src/hello.c`,
`sample-tree/`, …) are invisible from here, even though they live in
the same store.

---

## 8. Cross-view writes

Edit the shared README through the `programming` mount:

```sh
echo 'EDITED THROUGH PROGRAMMING' > ~/mnt/programming/README.md
```

Now read it through the `writing` mount — it's the same object:

```sh
cat ~/mnt/writing/docs/readme.md
```

```
EDITED THROUGH PROGRAMMING
```

One underlying file, many places it appears, all kept in sync.

---

## 9. Renames are local to one view

```sh
mv ~/mnt/programming/README.md ~/mnt/programming/README-renamed.md
ls ~/mnt/programming
```

```
README-renamed.md  sample-tree  src
```

But the `writing` view's mapping is untouched:

```sh
ls ~/mnt/writing/docs
cat ~/mnt/writing/docs/readme.md
```

```
readme.md
EDITED THROUGH PROGRAMMING
```

And `object paths` reflects the new state:

```sh
viewfs object paths "$README"
```

```
object 5fef5374...:
  programming:/README-renamed.md
  writing:/docs/readme.md
```

---

## 10. Importing without a mount

You don't have to mount a view to add content. `viewfs object import`
reads a host file straight into the store, and `--into VIEW:PATH`
optionally attaches the resulting object to one or more views in the
same step:

```sh
echo '# Project Notes' > /tmp/notes.md

viewfs object import /tmp/notes.md \
    --into programming:/notes-from-cli.md \
    --into writing:/articles/project-notes.md
```

This is the CLI-only equivalent of `cp`. It's most useful when:

- you want to drop the same file into several views at once,
- you're writing a script that loads many files and prefers not to
  mount/unmount,
- you want to register content before deciding which views (if any)
  will surface it.

Imported objects keep the host path in their `source_path` column, so
`viewfs object list` shows them with the original path instead of
`-`. `viewfs view add VIEW OBJECT VIEW_PATH` and `viewfs view remove
VIEW VIEW_PATH` adjust mappings after the fact.

---

## 11. Tags and attributes

ViewFS lets you attach arbitrary metadata to objects, queryable from
both the CLI and through FUSE extended attributes.

### CLI side

```sh
HELLO=$(viewfs object id programming /src/hello.c)

viewfs tag add  "$HELLO" project:compiler
viewfs tag add  "$HELLO" language:c
viewfs tag list "$HELLO"

viewfs attr set "$HELLO" author 'blake'
viewfs attr get "$HELLO"
```

```
tags of 1db84d28...:
  language:c
  project:compiler

Set 1db84d28....author = blake
attributes of 1db84d28...:
  author                   blake
```

### Find by tag or attribute

```sh
viewfs find --tag project:compiler
viewfs find --attr author=blake
```

Both list the matching objects with their kind, size, and source path.

### Same metadata, through FUSE xattrs

```sh
setfattr -n user.viewfs.author -v 'someone-else' ~/mnt/programming/src/hello.c
getfattr -d ~/mnt/programming/src/hello.c
```

The xattr namespace `user.viewfs.<key>` lives in the same table as
`viewfs attr`, so you can see the change either way:

```sh
viewfs attr get "$HELLO"
```

```
attributes of 1db84d28...:
  author                   someone-else
```

Only the `user.viewfs.` prefix is honored; other xattr names return
`ENOTSUP`.

---

## 12. Populating views from tags

If you'd like a view to materialize automatically from a tag:

```sh
viewfs view create archive

# Tag a second object so there's more than one thing to populate from:
NOTES=$(viewfs object id programming /sample-tree/notes.txt)
viewfs tag add "$NOTES" project:compiler

viewfs view populate archive --tag project:compiler --under /by-tag/compiler
```

```
  + archive:/by-tag/compiler/hello.c       -> 1db84d28...
  + archive:/by-tag/compiler/notes.txt     -> 92a01122...
Populated 2, skipped 0, failed 0 (...)
```

Mount the new view and you'll see those entries:

```sh
mkdir -p ~/mnt/archive
viewfs mount --view archive ~/mnt/archive
ls ~/mnt/archive/by-tag/compiler
```

Conflicts (re-running populate) are skipped with a warning rather than
aborting the batch.

---

## 13. Symbolic links

```sh
ln -s /src/hello.c ~/mnt/programming/hello-link
ls -l ~/mnt/programming/hello-link
readlink ~/mnt/programming/hello-link
```

```
lrwxrwxrwx 1 you you 12 ... hello-link -> /src/hello.c
/src/hello.c
```

A symlink target is stored as an opaque string. **The kernel resolves
the link in your filesystem namespace, so an absolute target like
`/etc/passwd` escapes the view** — this is the same as any other FUSE
filesystem. If hard isolation matters, run ViewFS inside a mount
namespace or container.

---

## 14. Checking for inconsistencies

ViewFS is built so that the only "expected" inconsistencies are
orphan objects (an object whose last mapping was removed). Everything
else (missing content files, size mismatches, orphan files on disk
without a DB row) means something has gone wrong — usually because
someone touched the backing store directly.

```sh
viewfs check
```

```
[1/3] DB integrity:
  orphan objects (no mappings):       0
  mappings with dangling object_id:   0
  mappings violating dir-invariant:   0
[2/3] Content <-> object cross-check:
  objects with missing content file:  0
  objects with size mismatch:         0
  content files with no object row:   0
[3/3] Schema version:
  DB schema version:   2
  Binary expects:      2
  OK
[4/4] Checksum coverage:
  file objects:                       4
  with checksum:                      4
  without checksum:                   0

Store is consistent.
```

The fourth phase reports how many file objects have a SHA-256 checksum
recorded vs. NULL. See §15 below for the policy that governs when
those are kept fresh vs. nulled.

To simulate a problem, plant a random file under `objects/`:

```sh
echo junk > $VIEWFS_STORE/objects/00/00000000000000000000000000000001
viewfs check
```

```
...
  content files with no object row:   1
Found 1 issue class(es). See above.
```

`--fix` removes only that kind of unambiguous garbage:

```sh
viewfs check --fix
```

It will never delete objects or modify mappings — that's deliberate.

### Checksum maintenance

Every file object has a SHA-256 in the `checksum` column. The daemon
tries to keep it correct without re-hashing the whole file on every
close:

- **Pure-append writes update the hash incrementally.** When you open
  a file for write and only ever write at the current end of file,
  the daemon advances a live SHA-256 stream and persists the new
  digest (plus a 112-byte resumable state) when you `close(2)`. The
  next open re-loads that state from the DB, so a
  `for i in …; do echo $i >> log; done` loop maintains the hash at
  O(bytes appended) total cost, not O(file × iterations).
- **Random writes / truncate-shrink null the column.** Anything that
  isn't a pure append loses the running hash; the daemon clears both
  `checksum` and `checksum_state` on close so the column doesn't lie.
- **`viewfs object show <id>`** prints the current checksum, with the
  marker `(resumable)` when the intermediate state is also stored.

Two flags repair after the fact:

```sh
viewfs check --fill-checksums      # compute hash + state for every NULL one
viewfs check --verify-checksums    # re-hash every non-NULL and report mismatches
```

`--verify-checksums` adds two extra lines to the phase-4 output:
`verified ok` and `mismatches`. A non-zero mismatch count is treated
as an issue class (non-zero exit).

---

## 15. Unmounting

```sh
viewfs unmount ~/mnt/programming
viewfs unmount ~/mnt/writing
viewfs unmount ~/mnt/archive
```

`viewfs unmount` is a thin wrapper around `fusermount3 -u`. After it
returns, the daemon has exited and its `daemons/<view>.pid` file is
gone.

Your mappings, objects, content, attributes, and tags all persist in
the store, ready for the next mount.

---

## 16. Reorganizing

You can change the structure of a view without touching anyone else's:

```sh
viewfs view remove writing /docs/readme.md   # detach the mapping
viewfs view add    writing "$README" /pinned/readme.md
```

Removing the *last* mapping for an object doesn't delete the content
— it leaves the object orphan (visible via `viewfs object list
--orphaned`). If you really mean it:

```sh
viewfs object delete "$NOTES"               # delete one by id/prefix
viewfs object delete --orphaned --dry-run   # preview a bulk cleanup
viewfs object delete --orphaned             # delete all orphans
```

`--orphaned` will only ever touch objects with zero mappings, so it
can't accidentally remove anything that's still reachable from a view.
`--dry-run` shows what *would* be deleted without acting.

To delete the whole view (and all its mappings, leaving the underlying
objects intact):

```sh
viewfs view delete archive
```

---

## 17. Inspecting from the outside

You can always poke at the PostgreSQL schema directly if you want to
see what's there:

```sh
psql -U "$VIEWFS_PG_USER" -d "$VIEWFS_PG_DATABASE" <<'SQL'
SET search_path TO viewfs;
SELECT view_name, view_path, entry_kind FROM mappings ORDER BY view_name, view_path;
SELECT object_id, size, kind FROM objects;
SELECT tag, count(*) FROM tags GROUP BY tag;
SQL
```

This is read-only — modifying the DB directly is a way to break things
that `viewfs check` will then loudly report.

---

## 18. Diagnostics during operation

If something is misbehaving, mount the view with `--foreground
--verbose` to see every callback as it happens:

```sh
viewfs mount --view programming --foreground --verbose ~/mnt/programming
```

```
viewfs-fuse[programming]: mounted view='programming' ro=0
viewfs-fuse[programming]: getattr /
viewfs-fuse[programming]: open /src/hello.c flags=0x8000
viewfs-fuse[programming]: read /src/hello.c sz=4096 off=0 -> 25
viewfs-fuse[programming]: release /src/hello.c fd=8
...
```

Each line includes the input path, key state, and outcome. Ctrl-C in
this terminal unmounts and exits.

---

## 19. A scripted end-to-end demo

If you'd rather see a guided demonstration than type along, the
project ships one:

```sh
./examples/demo.sh              # interactive — pauses between steps
./examples/demo.sh --unattended # CI-style — runs straight through
```

The demo uses its own dedicated schema and scratch dir so it never
conflicts with your tutorial store. It exercises the CLI-import path
from §10 rather than the mount-and-cp workflow.

---

## 20. Cleaning up after the tutorial

When you're done with the tutorial state:

```sh
viewfs unmount ~/mnt/programming  2>/dev/null
viewfs unmount ~/mnt/writing      2>/dev/null
viewfs unmount ~/mnt/archive      2>/dev/null

# Drop the PG schema (CLI doesn't currently have a dedicated command):
psql -U "$VIEWFS_PG_USER" -d "$VIEWFS_PG_DATABASE" -c 'DROP SCHEMA IF EXISTS viewfs CASCADE'

# Remove the backing store:
rm -rf "$VIEWFS_STORE"
rmdir ~/mnt/programming ~/mnt/writing ~/mnt/archive
```

---

## Where to go next

- `README.md` — top-level overview, status, build basics.
- `Design.md` — implementation design, phase by phase.
- `viewfs_fuse_prototype_spec.md` — the spec this prototype targets.
- `viewfs --help` and each `viewfs <subcommand> --help` — full CLI
  reference.
- `viewfs-fuse --help` — daemon-side options (`--ro`, `--verbose`,
  `--foreground`, …).

That's the tour. Have fun.
