# A guided tour of ViewFS

This document walks you through ViewFS by example. Each section is
short, has commands you can paste, and shows the output you should
expect. By the end you'll have created a backing store, populated it,
mounted views, written through them, queried metadata, and used the
diagnostic tools.

Before you start, make sure you've followed `INSTALL.md` so you have:

- `viewfs` and `viewfs-fuse` on your `$PATH` (or in the current
  directory),
- a running PostgreSQL with a database called `viewfs` (or whatever
  name you used),
- `VIEWFS_STORE` set to a path you don't mind clearing.

For this tutorial I'll use:

```sh
export VIEWFS_STORE=$HOME/tutorial-vfs
export VFS_PG='host=/var/run/postgresql user=postgres dbname=viewfs'
```

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

When you write through a mount, you change the object's content, so
*every* mapping that points at it sees the change. When you rename
inside a mount, only that view's mapping moves.

---

## 2. Initialize a backing store

```sh
viewfs init "$VIEWFS_STORE" --pg "$VFS_PG"
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
  conninfo:         host=/var/run/postgresql user=postgres dbname=viewfs
  views:            0
  objects:          0 (orphans: 0)
  mappings:         0
  content storage:  0 file(s), 0 B (0 bytes)
```

---

## 3. Create some views

Views are the named hierarchies your files will appear in. Make two:

```sh
viewfs view create programming --description 'source code'
viewfs view create writing     --description 'docs and notes'
viewfs view list
```

Output:

```
Created view 'programming'
Created view 'writing'
programming              source code
writing                  docs and notes
```

---

## 4. Import some files as objects

Importing reads a file from your host filesystem into ViewFS as a
content blob with a fresh object ID. The original file isn't touched.

```sh
echo 'int main(void){return 0;}' > /tmp/hello.c
echo '# Project Notes'            > /tmp/notes.md
echo '# README'                   > /tmp/readme.md

viewfs object import /tmp/hello.c
viewfs object import /tmp/notes.md
viewfs object import /tmp/readme.md

viewfs object list
```

`object list` prints (object IDs will differ):

```
1db84d28...                  file              25  mode=0644  /tmp/hello.c
276491147c428...             file              16  mode=0644  /tmp/notes.md
5fef537...                   file              10  mode=0644  /tmp/readme.md
```

You'll refer to objects by ID or by any unambiguous *prefix* (e.g. the
first 8 hex characters). Capture the ones we'll need:

```sh
HELLO=$( viewfs object list | awk '/hello.c/{print $1}')
NOTES=$( viewfs object list | awk '/notes/  {print $1}')
README=$(viewfs object list | awk '/readme/ {print $1}')
```

---

## 5. Map objects into views

Each mapping says "object `$ID` appears at view-path `$P` in view `$V`."
Parent directories are auto-created as you go.

```sh
viewfs view add programming "$HELLO"  /src/hello.c
viewfs view add writing     "$NOTES"  /articles/project-notes.md

# Same object in BOTH views under different paths:
viewfs view add programming "$README" /README.md
viewfs view add writing     "$README" /docs/readme.md
```

Inspect:

```sh
viewfs view show programming
```

```
view 'programming':
  file   /README.md                               -> 5fef5374...
  dir    /src
  file   /src/hello.c                             -> 1db84d28...
```

```sh
viewfs view show writing
```

```
view 'writing':
  dir    /articles
  file   /articles/project-notes.md               -> 27649114...
  dir    /docs
  file   /docs/readme.md                          -> 5fef5374...
```

To see every view-path for a single object:

```sh
viewfs object paths "$README"
```

```
object 5fef5374...:
  programming:/README.md
  writing:/docs/readme.md
```

The shared README has two view-paths in two views, but it's one object
on disk. We'll exploit that in a moment.

---

## 6. Mount a view

A mount exposes one view at a directory of your choice. Mount both:

```sh
mkdir -p ~/mnt/programming ~/mnt/writing

viewfs mount --view programming ~/mnt/programming
viewfs mount --view writing     ~/mnt/writing
```

`viewfs mount` spawns the daemon, which backgrounds itself and starts
serving. Pass `--foreground` if you want the daemon attached to your
terminal (useful with `--verbose` for per-callback tracing).

Confirm the mounts:

```sh
mount | grep viewfs
```

```
viewfs:programming on /home/you/mnt/programming type fuse (rw,...,default_permissions)
viewfs:writing     on /home/you/mnt/writing     type fuse (rw,...,default_permissions)
```

---

## 7. Browse and read like any directory

The mountpoint behaves like a normal Linux directory:

```sh
ls -l ~/mnt/programming
ls -l ~/mnt/writing
cat   ~/mnt/programming/src/hello.c
cat   ~/mnt/writing/docs/readme.md
```

Note that `~/mnt/programming` shows only the mappings under the
`programming` view — `articles/`, `docs/`, and `project-notes.md`
from the `writing` view are invisible here, even though they're in
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

This is the central feature: one underlying file, many places it
appears, all kept in sync.

---

## 9. Renames are local to one view

```sh
mv ~/mnt/programming/README.md ~/mnt/programming/README-renamed.md
ls ~/mnt/programming
```

```
README-renamed.md
src
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

## 10. Creating new files through a mount

A mounted view is fully writable. Creating, writing, and deleting all
work as expected:

```sh
echo 'new through FUSE' > ~/mnt/programming/extra.txt
mkdir ~/mnt/programming/subproj
echo 'main' > ~/mnt/programming/subproj/main.c

ls -R ~/mnt/programming
```

```
.:
README-renamed.md  extra.txt  src  subproj

./src:
hello.c

./subproj:
main.c
```

The new object shows up in the CLI immediately:

```sh
viewfs view show programming | tail -3
```

---

## 11. Tags and attributes

ViewFS lets you attach arbitrary metadata to objects, queryable from
both the CLI and through FUSE extended attributes.

### CLI side

```sh
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
viewfs tag add "$NOTES"  project:compiler
viewfs view populate archive --tag project:compiler --under /by-tag/compiler
```

```
  + archive:/by-tag/compiler/hello.c       -> 1db84d28...
  + archive:/by-tag/compiler/notes.md      -> 27649114...
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
lrwxrwxrwx 1 you you 10 ... hello-link -> /src/hello.c
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
  DB schema version:   1
  Binary expects:      1
  OK

Store is consistent.
```

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
psql "$VFS_PG" <<'SQL'
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
conflicts with your tutorial store.

---

## 20. Cleaning up after the tutorial

When you're done with the tutorial state:

```sh
viewfs unmount ~/mnt/programming  2>/dev/null
viewfs unmount ~/mnt/writing      2>/dev/null
viewfs unmount ~/mnt/archive      2>/dev/null

# Drop the PG schema (CLI doesn't currently have a dedicated command):
psql "$VFS_PG" -c 'DROP SCHEMA IF EXISTS viewfs CASCADE'

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
