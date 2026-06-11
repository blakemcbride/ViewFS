# View-Based File System FUSE Prototype Specification

> **Revision — property-driven model.** This specification describes the
> prototype as it now exists: view membership is **computed from
> properties**, not stored as explicit per-view path mappings. A directory
> carries a set of property/value pairs (a *filter*); a file appears in a
> directory exactly when the file's properties are a **superset** of that
> directory's *effective* filter. An earlier revision of this document
> specified explicit path mappings and per-view filenames; that mechanism
> has been replaced. The current schema and design are described in
> `Design.md`.

## 1. Purpose

Build a Linux FUSE prototype for a view-based file system model.

The prototype shall demonstrate a file system in which users interact with
named, task-specific views instead of a single fixed hierarchy. Each view
presents a normal directory tree to applications, but the files visible
inside a directory are computed from properties: a directory holds a set of
property/value pairs, and the files it contains are exactly the objects whose
properties satisfy that set. Existing applications shall access files through
ordinary POSIX file operations without modification.

This document specifies goals, functional requirements, constraints, and
acceptance criteria for an initial proof-of-concept implementation. It
intentionally does not prescribe a final kernel design, final on-disk
format, or long-term production architecture.

## 2. Background

Traditional operating systems expose files primarily through a hierarchical
namespace. That model has served well for decades, but becomes difficult to
navigate and manage as systems contain hundreds of thousands of files. A
single hierarchy is also rigid: different tasks may require different
organizational structures. Symbolic links and hard links help, but they do
not fully solve the problem of task-specific organization.

The proposed model introduces **views**. A view is a named, use-case-specific
hierarchy. Each directory in a view carries a set of property/value pairs,
and the files visible in that directory are computed: an object appears
wherever its own properties are a superset of the directory's effective
filter. A file therefore appears in any number of directories and views
simultaneously — wherever it matches — with no copying and no stored
placement records. The organizing act is assigning properties to objects and
filters to directories.

The FUSE prototype shall implement enough of this model to test the core
user experience and semantics.

## 3. Definitions

### 3.1 Backing Store

The ordinary host directory tree used by the prototype to store real file
contents, plus its configuration. Object and view metadata live in a
PostgreSQL schema (see §10). The backing store is not the user-facing
filesystem; it is an implementation detail.

### 3.2 Object

A persistent file-like entity with stable identity independent of any view or
path. An object carries content, an intrinsic display **name**, and a set of
**properties**. The same object surfaces in every directory whose filter its
properties satisfy.

### 3.3 View

A named user-facing filesystem hierarchy: a tree of directories, each with an
attached property filter. A view exposes objects through ordinary directories
and filenames; the file contents of each directory are computed.

### 3.4 Directory Filter

The set of property/value pairs attached to a directory. Each pair carries a
**flow** flag (see §3.8). A directory with an empty filter matches every
object (the empty set is a subset of everything), so a view's root and any
freshly created directory list all objects until pairs are attached.

### 3.5 Property

A `(key, value)` metadata pair associated with an object. Properties are
**multi-valued**: a key may hold several values on one object. Properties are
the sole driver of view membership.

### 3.6 Tag

A shorthand for a property whose key is `tag`. A tag is not a separate kind
of metadata; `tag=draft` is an ordinary property.

### 3.7 Effective Filter

The filter actually applied to a directory: its own pairs plus every pair on
an ancestor directory that is marked to flow (§3.8). An object is a member of
a directory iff its property set is a superset of the directory's effective
filter (relational division: the object has every required pair, possibly
more).

### 3.8 Flow

A boolean on each directory-filter pair. A pair with `flow = false` (the
default) constrains only its own directory (independent nesting). A pair with
`flow = true` also applies to all descendant directories, making each
descendant a strict subset of the ancestor (cumulative nesting).

### 3.9 Active View

The view currently exposed at a mounted FUSE mount point. A process using
that mount point shall only see objects whose properties match the
directories of that view.

### 3.10 Source Path

The original host path an object was imported from, if any. Informational;
not visible inside a view.

## 4. Project Scope

### 4.1 In Scope

The prototype shall include:

1. A FUSE filesystem mountable on Linux.
2. Support for multiple named views.
3. Normal directory traversal within a mounted active view.
4. File visibility computed from properties and per-directory filters.
5. File identity independent of view, directory, and name.
6. The ability for one object to appear in multiple directories and views
   at once, by property match.
7. Per-directory property filters, with a per-pair flow flag for cumulative
   nesting.
8. Multi-valued object properties.
9. Persistent view, directory, filter, object, and property metadata.
10. Basic file operations through POSIX-compatible interfaces, including
    create (assigning the directory's properties) and move (swapping the
    source directory's pairs for the destination's).
11. A command-line administration tool.
12. Tests demonstrating the required behavior.

### 4.2 Out of Scope for Initial Prototype

The initial prototype shall not attempt to implement:

1. A kernel filesystem module.
2. A new block-device filesystem.
3. A production-ready on-disk format.
4. Full Linux discretionary access control replacement.
5. Mandatory access control integration.
6. SELinux/AppArmor integration.
7. Network filesystem support.
8. Distributed synchronization.
9. Multi-host consistency.
10. Full crash-consistency guarantees beyond ordinary safe metadata writes
    and `close(2)`-completed content writes.
11. Quotas. 12. Snapshots. 13. Deduplication. 14. Compression.
15. Encryption. 16. File versioning.
17. High-performance indexing for very large repositories.
18. Inotify correctness beyond what FUSE naturally provides.
19. Hard link semantic equivalence across all edge cases.
20. Production-grade package management.
21. A declarative view rule language beyond property-pair filters with flow.

## 5. Prototype Goals

The implementation shall demonstrate the following concepts:

1. A user can define multiple named views, each a tree of directories with
   property filters.
2. A user can mount one view and interact with it as a normal filesystem.
3. A program using ordinary file operations sees only the objects whose
   properties match the mounted view's directories.
4. The same object appears in every directory and view whose filter it
   satisfies — no copies, no stored placement.
5. A directory filter pair can flow to descendants, making a child a strict
   subset of its parent.
6. Creating a file inside a directory gives the new object that directory's
   effective properties; moving a file swaps the source directory's pairs
   for the destination's; both persist.
7. View, directory, filter, object, and property metadata survive unmount
   and remount.
8. Multi-valued properties can be assigned to objects and queried.
9. Membership can be inspected and changed through an administration tool by
   editing directory filters or object properties.

## 6. Non-Goals

The prototype shall not claim that FUSE alone provides complete process
security against all possible host filesystem access. It only restricts
access through the mounted FUSE view. A process that also has direct access
to the backing store, or that can connect to the PostgreSQL role, may access
those resources through ordinary mechanisms.

The prototype shall not attempt to replace mount namespaces, containers,
chroot, SELinux, AppArmor, or kernel-enforced sandboxing. It shall not define
the final design of a future Linux VFS extension, and shall not require
changes to existing application source code.

## 7. User-Facing Behavior

### 7.1 Mounting a View

The user shall be able to mount a named view at a mount point:

```sh
vfs mount programming ~/mnt/programming
```

After mounting, ordinary shell commands shall work (`cd`, `ls`, `cat`,
`mkdir`, `cp`, `mv`, `rm`, …).

### 7.2 View Isolation

When a view is mounted, a directory listing shall show that directory's child
directories plus the objects whose properties satisfy the directory's
effective filter. An object that does not match shall not be visible via
`readdir`, and shall not be openable by guessing a backing-store path. A `..`
traversal attempt shall not escape the mounted view.

### 7.3 Normal File Access

Existing programs shall be able to use the mounted view with ordinary file
operations: `open`, `read`, `write`, `close`, `stat`, `rename`, `unlink`,
`mkdir`, `rmdir`, `truncate`, and `fsync` (at least best-effort). The
prototype shall expose behavior close enough to a normal POSIX filesystem for
common command-line tools to work.

### 7.4 Multiple Views

The system shall support multiple named views, mountable at different mount
points simultaneously. The same object may appear in multiple views — wherever
its properties match each view's directory filters. A content change to a
shared object through one view shall be visible through another view after
ordinary cache effects resolve.

### 7.5 Names and Cross-View Appearance

An object has a single intrinsic name. It appears under that name in every
directory it matches, in every view. Different views surface the same object
through different *filters*, not different names.

When two distinct objects share a name within one directory, the mount shall
keep them individually addressable — e.g. by suffixing the displayed name
with a short object-id prefix (`report.txt~a1f8`). A uniquely-named object is
shown bare.

### 7.6 Adding Files Through a View

When a user creates a new file inside a directory of a mounted view, the
prototype shall:

1. Create a new object and store its content in the backing store.
2. Assign the directory's **effective** property pairs to the new object, so
   it satisfies that directory's filter and appears there.
3. Persist the object and its properties.
4. Ensure the new file is visible after unmount and remount.

A copy (`cp`) of an external file into a directory behaves the same way: a new
object that takes on the destination directory's pairs. The new object shall
not appear in unrelated directories unless its properties also match them.

### 7.7 Removing Files Through a View

When a user deletes a file through a mounted view (`unlink`), the prototype
shall delete the object and its content. Because membership is computed, the
object then disappears from every directory and view at once.

To remove a file from one directory **without** destroying it, the user shall
instead change the object's properties (via the administration tool, or by
`mv` to a directory whose filter it no longer matches) so it stops matching
that directory. The administration tool shall also provide explicit object
deletion and a way to list objects that match nothing meaningful (§15.3).

### 7.8 Renaming and Moving Files Within a View

Moving a file from one directory to another (`mv`) shall keep the same object
and shall: remove the source directory's effective pairs from the object, add
the destination directory's effective pairs, and retain all other properties.
The object's name shall become the destination basename. Object identity is
preserved; content is untouched.

Because membership is global to a view, a move may change where the object
appears in other directories too (any directory defined by the swapped pairs)
— this follows directly from the model and is intended.

Renaming a directory shall move that directory's subtree, carrying its filter
pairs (and all descendants') with it.

### 7.9 Directories

Directories are **explicit**, first-class rows forming a per-view tree with a
single root. `mkdir` creates a directory with an **empty** filter; the
administration tool attaches property pairs to it. `mkdir` and `rmdir` through
the mount persist in the active view. `rmdir` shall succeed only when the
directory has no child directories **and** no matching files (an empty filter
matches everything, so a directory must narrow its filter to nothing — or the
store must be empty — before it is removable). The README documents this.

### 7.10 Properties and Tags

The prototype shall allow arbitrary, multi-valued properties to be associated
with objects. Minimum required operations:

1. Add a `(key, value)` pair to an object.
2. List an object's properties.
3. Remove a single value, or all values, of a key.
4. List objects matching one or more property pairs.

A tag is a property with key `tag`; no separate tag commands are required.
The same `prop` command shall manage properties on a file (object) and the
filter on a directory; the *target* distinguishes them (an object id, a
`VIEW:DIR`, or a path/name resolved against the current directory in a mount).
Example command shapes:

```sh
vfs prop set    language=C TARGET      # TARGET = object id, VIEW:DIR, or path
vfs prop list   TARGET
vfs prop unset  language TARGET
vfs find --prop language=C
vfs find --prop tag=draft --prop author=blake   # AND across pairs
```

### 7.11 Membership Management

Membership is changed by editing directory filters or object properties — not
by adding/removing placement rows. The administration tool shall provide:

```sh
vfs dir mkdir   VIEW DIR
vfs dir rmdir   VIEW DIR
vfs dir ls      VIEW DIR             # computed contents
vfs prop set    KEY=VALUE VIEW:DIR [--flow]   # attach a filter pair
vfs prop unset  KEY[=VALUE] VIEW:DIR
vfs prop list   VIEW:DIR [--effective]
```

To make an object appear in `VIEW:DIR`, give it that directory's pairs — by
setting properties directly, or via `object import --into VIEW:DIR`, which
assigns the directory's effective pairs to the imported object.

### 7.12 View Listing and Inspection

The administration tool shall support:

```sh
vfs view list
vfs view create VIEW [DESCRIPTION]
vfs view delete VIEW
vfs view show   VIEW           # the directory tree
vfs object show OBJECT_ID
vfs object name OBJECT_ID [NEWNAME]
```

There is no "paths for an object across views" command, because an object's
locations are computed; `vfs dir ls` and `vfs find --prop` answer
"where does this appear" by query.

## 8. View Definition Requirements

### 8.1 Property-Driven Membership

The prototype's view-definition mechanism is the **directory filter**: each
directory carries a set of `(key, value, flow)` pairs. A directory's file
members are exactly the objects whose properties are a superset of the
directory's effective filter (own pairs plus flowed ancestor pairs). There is
no explicit per-file placement table; membership is always computed.

A directory record shall include its view, canonical directory path, parent
path, name, mode bits, and timestamps. A filter pair shall include its view,
directory path, key, value, flow flag, and timestamps.

### 8.2 Multi-Valued and Flow Semantics

A directory may list several values for one key (the object must have all of
them — AND). A pair marked `flow` applies to descendant directories,
expressing cumulative nesting; unmarked pairs are independent. Both forms
shall be supported and shall be inspectable (`prop list VIEW:DIR --effective`
shows inherited pairs and their source directory).

### 8.3 Membership Is Dynamic

The README shall state that view membership is **dynamically computed from
object properties and per-directory filters** (it is option 3 — "dynamically
queried from attributes" — of the earlier revision's enumeration). No
materialization step is required; editing a property or a filter changes
membership immediately.

## 9. Object Identity Requirements

Each object shall have a stable internal identifier that does not depend on
backing-store path, view, directory, name, or properties. The administration
tool shall report an object's ID. Each object also has an intrinsic display
name, which may be changed without affecting identity. It is acceptable for
the FUSE mount to expose inode numbers derived per-mount, provided behavior is
stable enough for normal tools.

## 10. Storage Requirements

### 10.1 Backing Store Layout

The backing store directory shall contain file contents and configuration.
Object, property, view, directory, and filter metadata are held in PostgreSQL.
The backing store location shall be configurable:

```sh
vfs init STORE_PATH
```

`vfs init` shall create the PostgreSQL database if it does not already
exist (connecting to a maintenance database to issue `CREATE DATABASE`),
provided the connecting role has the privilege to do so, and then create the
schema and apply migrations. Tearing the system down (the `DestroyAll.sh` helper)
correspondingly drops the whole database.

### 10.2 Metadata Persistence

Metadata shall survive unmounting, remounting, restarting the FUSE daemon,
and rebooting (assuming the backing store and database are persistent).

### 10.3 Metadata Format

Metadata is stored in **PostgreSQL** via libpq, in a single configurable
schema. (This revises the earlier prototype guidance, which suggested a
local embedded store and avoiding a database server; the implementation uses
PostgreSQL for its indexed relational-division membership queries and
`LISTEN/NOTIFY` cache invalidation.) The schema is documented in
`src/libviewfs/migrations/0001_init.sql` and explained in `Design.md`.

### 10.4 Content Storage

File contents shall be stored under the backing store, named by object ID and
sharded one level by the first two hex characters of the ID. The sharded
layout shall never be exposed through a mounted view.

## 11. Security Requirements

### 11.1 Mounted View Boundary

The FUSE filesystem shall not expose, through normal mounted-path operations,
objects that do not match the active view's directories. The implementation
shall defend against `..` traversal escaping the view, escaping symlink
traversal (where symlinks are implemented), absolute backing-store paths
being interpreted as view paths, path-normalization bugs, and opening
unmatched objects through guessed names.

### 11.2 Backing Store Protection

The README shall warn that users must not give sandboxed processes direct
access to the backing store or to the PostgreSQL role if they want view
isolation. The backing store and database role are assumed trusted and
controlled by the user running the FUSE daemon.

### 11.3 Process-Level Isolation Limits

The README shall document that FUSE view isolation applies to accesses through
the FUSE mount only, and shall recommend combining the mount with mount
namespaces, containers, `chroot`, bubblewrap, or SELinux/AppArmor for stronger
isolation. The implementation need not configure these.

### 11.4 Permissions

The prototype shall support basic POSIX mode bits for files (on objects) and
directories (on directory rows): store them, return them through `getattr`,
respect read-only mounts, and preserve executable bits. Ownership may be
simplified to the invoking user in the initial prototype.

## 12. FUSE Operation Requirements

### 12.1 Required Operations

`getattr`, `readdir`, `open`, `read`, `write`, `create`, `mkdir`, `unlink`,
`rmdir`, `rename`, `truncate`, `utimens`, `flush` (or equivalent), `fsync`
(or documented best-effort), `release`.

### 12.2 Recommended Operations

`statfs`, `chmod`, `chown` (even if simplified), `readlink`, `symlink`,
extended attributes, `access`. Extended attributes under `user.viewfs.` map
onto object properties; through xattrs a key is single-valued (set replaces
all values of the key).

### 12.3 Path Resolution

All path resolution shall be relative to the active view and canonicalized
before lookup. Canonicalization shall normalize or reject duplicate slashes,
`.` components, `..` components (no escape past root), empty components except
root, and invalid filenames. A canonical directory path resolves to a
directory row; otherwise the final component is matched as an object name
among the parent directory's members (honoring the `name~idprefix`
disambiguation form of §7.5).

### 12.4 Error Behavior

Normal POSIX-style errors: `ENOENT` for paths not matching the active view,
`EEXIST` when creating a directory that already exists, `ENOTDIR`/`EISDIR` as
appropriate, `ENOTEMPTY` when removing a directory that has child dirs or
matching files, `EACCES` for permission failures, `EROFS` on a read-only
mount.

## 13. Command-Line Tool Requirements

The project shall provide a tool named `vfs`. It shall support at least:

### 13.1 Repository

```sh
vfs init STORE_PATH
vfs status
vfs check
```

### 13.2 Views

```sh
vfs view create VIEW [DESCRIPTION]
vfs view list
vfs view delete VIEW
vfs view show   VIEW
```

### 13.3 Mounting

```sh
vfs mount   VIEW MOUNTPOINT [--ro] [--foreground] [--verbose]
vfs unmount MOUNTPOINT          # may wrap fusermount3 -u
```

### 13.4 Directories

```sh
vfs dir mkdir   [VIEW] DIR
vfs dir rmdir   [VIEW] DIR
vfs dir ls      [[VIEW] DIR]
```

Directory *filters* are managed with `prop` (§13.6), addressing the directory
as `VIEW:DIR` (or by path/cwd inside a mount).

### 13.5 Objects

```sh
vfs object import SOURCE_PATH [--name NAME] [--into VIEW:DIR]...
vfs object show   OBJECT_ID
vfs object name   OBJECT_ID [NEWNAME]
vfs object copy   OBJECT_ID VIEW:DIR
vfs object id     VIEW VIEW_PATH
vfs object list   [--orphaned]
vfs object delete OBJECT_ID
vfs object delete --orphaned [--dry-run]
```

### 13.6 Properties (files and directory filters)

`TARGET` is an object id, a `VIEW:DIR`, or — inside a mount — a path/name.
Targets come last (like `chmod`) and a list may be given; with none, each
subcommand defaults to the current directory. A property is written as one
`KEY=VALUE` token (for `unset` the value is optional; a bare `KEY` removes
every value). `--flow`/`--effective` apply to directory targets only.

```sh
vfs prop set    KEY=VALUE   [TARGET...] [--flow]
vfs prop unset  KEY[=VALUE] [TARGET...]
vfs prop list   [TARGET...] [--effective]
vfs find --prop KEY[=VALUE] [--prop KEY[=VALUE]]...
```

## 14. Data Model Requirements

The implementation shall maintain equivalent information to the following
entities.

### 14.1 Object Record

Object ID; file type (`file`/`symlink`); intrinsic name; content reference;
size; created/modified/accessed timestamps; mode bits; optional owner/group;
optional checksum (and resumable checksum state); optional source path;
symlink target (for symlinks).

### 14.2 Property Record

Object ID; key; value; timestamps. Multi-valued: the primary key includes the
value, so a key may carry several values per object.

### 14.3 View Record

View name; created/modified timestamps; optional description.

### 14.4 Directory Record

View name; canonical directory path; parent path; name; mode bits;
created/modified timestamps. Directories form a per-view tree with a single
root.

### 14.5 Directory Filter Record

View name; directory path; key; value; flow flag; timestamps. Referenced to
its directory; deleting or renaming the directory cascades to its filter
pairs.

## 15. Consistency Requirements

### 15.1 Shared Object Content

If an object matches multiple directories or views, all references are to the
same content. Writing through one path updates the object; other views observe
the updated content.

### 15.2 Independent Views

Each view's membership is a function of that view's directory filters and the
objects' properties. Editing one view's directory filters does not change
another view's directories. (Editing an object's properties may, however,
change where it appears across all views — that is inherent to the model.)

### 15.3 Loose Objects

The implementation shall provide a way to list objects that carry no
properties (and therefore match only empty-filter directories such as view
roots):

```sh
vfs object list --orphaned
```

It shall provide explicit object deletion; deletion of object content shall be
explicit (`unlink` through the mount also deletes — §7.7):

```sh
vfs object delete OBJECT_ID
```

## 16. Logging and Diagnostics

The prototype shall provide: a verbose mode for the FUSE daemon; clear CLI
error messages; a way to inspect backing-store/database status (`viewfs
status`); a metadata consistency check (`vfs check`) covering DB integrity
(propertyless objects, unreachable or structurally inconsistent directories,
orphan property rows), content↔object cross-checks, schema version, and
checksum coverage; and logs sufficient to debug path resolution.

## 17. Testing Requirements

The project shall include automated tests covering at minimum:

1. Initializing a backing store; `status`/`check` on a fresh store.
2. Creating views and directories.
3. Attaching directory filters, independent vs. flowed.
4. Importing objects and assigning properties (incl. `--into`).
5. Computed membership: superset match, multi-value AND, empty-filter
   matches everything.
6. `find --prop` across multiple pairs (AND).
7. Mounting a view; listing a directory's child dirs and matching files.
8. Confirming non-matching objects are not listed.
9. Reading a matching file; `ENOENT` for a non-matching path.
10. The same object surfacing in two views by property match.
11. Writing a shared object through one view and reading it through another.
12. Creating a file inside a directory and seeing it gain that directory's
    properties.
13. Moving a file between directories (property swap) and `rm` deleting the
    object.
14. Two same-named objects in one directory remaining individually
    addressable (`name~idprefix`).
15. Persistence of views, directories, filters, and properties across
    unmount and remount.
16. `vfs check` detecting a deleted content file.
17. Power-loss resilience: after a `close(2)` and a hard kill of the daemon,
    the store is consistent and the bytes survive remount.
18. `..` traversal handled safely.

Tests may use temporary directories, an isolated PostgreSQL schema, and
temporary FUSE mount points.

## 18. Demonstration Scenario

The completed prototype shall include a scripted demonstration that:

1. Initializes a new backing store.
2. Creates a view and a directory tree of property filters, including a
   flowed pair (so a child directory is a strict subset of its parent).
3. Imports files and assigns properties, including one object that matches
   several directories at once.
4. Shows computed membership: the same object appearing in multiple
   directories with no copies.
5. Queries objects with `find --prop`.
6. Mounts the view and browses it with ordinary commands.
7. Creates a file inside a filtered directory and shows it gained that
   directory's properties.
8. Re-filters the same objects through a second view.
9. Unmounts and remounts, showing everything persists.

## 19. Build and Platform Requirements

The prototype targets Linux and FUSE 3. Build instructions shall include
Fedora-oriented setup. Expected Fedora dependencies:

```sh
sudo dnf install fuse3 fuse3-devel libpq libpq-devel \
                 gcc make pkgconf-pkg-config postgresql
```

The project shall build with `make`, producing the `vfs` CLI and the
`vfs-fuse` daemon.

## 20. Documentation Requirements

The README shall explain: what the prototype demonstrates and does not; how to
build it; how to initialize a backing store; how to create views and
directories; how to attach property filters and assign object properties; how
files become members by property match (including flow); how to import,
mount, and unmount; how shared objects behave; how to use properties and
xattrs; security limitations; and how to run the tests and the demonstration.
The README shall state that the prototype is experimental and should not be
used for irreplaceable data, and shall describe the durability contract
(writes completed by `close(2)` are durable; in-flight writes are not).

## 21. Acceptance Criteria

The implementation is acceptable when all of the following are true:

1. A user can initialize a backing store.
2. A user can create at least two views, each with a directory tree.
3. A user can import files as objects and give them properties.
4. A user can attach property filters to directories, including flowed pairs.
5. A user can mount a view with FUSE.
6. The mounted view behaves like a normal directory tree for basic shell
   commands.
7. Objects whose properties do not match a directory are not visible there.
8. The same object is visible in every directory and view whose filter it
   satisfies, with no copies.
9. Creating a file in a directory gives it that directory's properties;
   moving a file swaps source pairs for destination pairs.
10. Content changes through one view are reflected through other views
    referencing the same object.
11. Editing one view's directory filters does not change another view's
    directories.
12. `unlink` deletes the object; changing properties removes a file from a
    directory without destroying it.
13. Views, directories, filters, and properties persist across unmount and
    remount.
14. Multi-valued properties can be assigned and queried, including AND
    across pairs and the flow flag.
15. Automated tests cover the required behavior.
16. The README documents build, usage, limitations, and demo steps.

## 22. Suggested Repository Layout

```text
viewfs/
  README.md
  Design.md                  (authoritative model + design rationale)
  viewfs_fuse_prototype_spec.md
  Makefile
  include/viewfs/viewfs.h
  src/
    libviewfs/   (store, objects, object_props, view_dirs, members, find, …)
    cli/         (viewfs: view, dir, object, prop, find, mount, check, …)
    fuse/        (vfs-fuse daemon: membership-driven callbacks)
  tests/
    unit/        (canonicalizer, object id, property model)
    integration/ (end-to-end: dirs/props, mount read/write, dup names, check, crash)
  examples/
    demo.sh
```

The requirements in this specification are authoritative over the suggested
layout.

## 23. Important Design Constraints

The implementation shall preserve these core conceptual constraints:

1. Views are first-class named entities — separate user-facing hierarchies.
2. Object identity is independent of view, directory, name, and properties.
3. A directory is a property **filter**; its file contents are computed.
4. An object appears in a directory iff its properties are a superset of the
   directory's effective filter.
5. The same object may appear in many directories and views at once, by
   match, with no copies.
6. Properties (and tags, which are just properties) belong to objects, not
   paths; they are multi-valued.
7. A directory filter pair may flow to descendants (cumulative) or not
   (independent).
8. Programs access files normally through paths; a mounted view shows only
   its matching files.
9. The prototype is layered over an existing filesystem through FUSE, with
   metadata in PostgreSQL.

## 24. Handoff Instruction for Claude Code

Implement the project described in this specification as a working Linux
FUSE 3 prototype. Favor a small, understandable, testable implementation over
a complex or highly optimized one. Do not implement kernel code, design a
production filesystem, or add major unrelated features. Where this
specification leaves choices open, choose the simplest option that satisfies
the acceptance criteria and document it in the README.
