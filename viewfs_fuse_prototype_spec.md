# View-Based File System FUSE Prototype Specification

## 1. Purpose

Build a Linux FUSE prototype for a view-based file system model.

The prototype shall demonstrate a file system in which users interact with named, task-specific views instead of a single fixed hierarchy. Each view presents a normal directory tree to applications, but the files visible inside that tree are only the subset assigned to that view. Existing applications shall access files through ordinary POSIX file operations without modification.

This document specifies goals, functional requirements, constraints, and acceptance criteria for an initial proof-of-concept implementation. It intentionally does not prescribe a final kernel design, final on-disk format, or long-term production architecture.

## 2. Background

Traditional operating systems expose files primarily through a hierarchical namespace. That model has served well for decades, but becomes difficult to navigate and manage as systems contain hundreds of thousands of files. A single hierarchy is also rigid: different tasks may require different organizational structures. Symbolic links and hard links help, but they do not fully solve the problem of task-specific organization.

The proposed model introduces **views**. A view is a named, use-case-specific hierarchy containing a subset of all files known to the system. A file may appear in multiple views, may appear under different paths in different views, and may have different per-view names. Files and groups of files may have arbitrary attributes or tags, and views may use these attributes to determine membership.

The FUSE prototype shall implement enough of this model to test the core user experience and semantics.

## 3. Definitions

### 3.1 Backing Store

The ordinary host directory tree used by the prototype to store real file contents and metadata.

The backing store is not the user-facing filesystem. It is an implementation detail used by the prototype.

### 3.2 Object

A persistent file-like entity with stable identity independent of any particular view path.

An object may be represented in one or more views. Multiple view paths may refer to the same object.

### 3.3 View

A named user-facing filesystem hierarchy.

Each view exposes a subset of objects through ordinary directories and filenames. A view may organize the same object differently than other views.

### 3.4 View Path

The path of a file or directory as it appears inside a specific view.

The same object may have different view paths in different views.

### 3.5 Attribute

A key-value metadata item associated with an object.

Attributes may be used to select objects for a view.

### 3.6 Tag

A shorthand form of attribute used for classification.

A tag may be represented internally as an attribute with a boolean or set value.

### 3.7 Active View

The view currently exposed at a mounted FUSE mount point.

A process using that mount point shall only see files and directories visible in that view.

### 3.8 Source Path

The original or backing-store path associated with an object, if any.

The source path is not necessarily visible inside a view.

## 4. Project Scope

### 4.1 In Scope

The prototype shall include:

1. A FUSE filesystem mountable on Linux.
2. Support for multiple named views.
3. Normal directory traversal within a mounted active view.
4. File visibility limited to the active view.
5. File identity independent of view path.
6. The ability for one file object to appear in multiple views.
7. The ability for one file object to have different names or paths in different views.
8. Persistent view definitions.
9. Persistent object metadata.
10. Persistent per-view path mappings.
11. Basic file operations through POSIX-compatible interfaces.
12. A simple command-line administration tool.
13. Tests demonstrating the required behavior.

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
10. Full crash-consistency guarantees beyond ordinary safe metadata writes.
11. Quotas.
12. Snapshots.
13. Deduplication.
14. Compression.
15. Encryption.
16. File versioning.
17. High-performance indexing for very large repositories.
18. Inotify correctness beyond what FUSE naturally provides.
19. Hard link semantic equivalence across all edge cases.
20. Production-grade package management.

## 5. Prototype Goals

The implementation shall demonstrate the following concepts:

1. A user can define multiple named views.
2. A user can mount one view and interact with it as a normal filesystem.
3. A program using ordinary file operations sees only the files exposed by the mounted view.
4. The same underlying file can appear in multiple views.
5. The same underlying file can appear under different names or directories in different views.
6. Adding, renaming, removing, or moving files within a view persists in that view.
7. View metadata survives unmount and remount.
8. Attribute or tag metadata can be assigned to files.
9. View membership can be inspected and changed through an administration tool.

## 6. Non-Goals

The prototype shall not claim that FUSE alone provides complete process security against all possible host filesystem access. The prototype only restricts access through the mounted FUSE view. A process that also has direct access to the backing store or other host paths may still access those paths through ordinary operating system mechanisms.

The prototype shall not attempt to replace mount namespaces, containers, chroot, SELinux, AppArmor, or kernel-enforced sandboxing.

The prototype shall not define the final design of a future Linux VFS extension.

The prototype shall not require changes to existing application source code.

## 7. User-Facing Behavior

### 7.1 Mounting a View

The user shall be able to mount a named view at a mount point.

Example command shape:

```sh
viewfs mount programming ~/mnt/programming
```

After mounting, ordinary shell commands shall work:

```sh
cd ~/mnt/programming
ls
cat README.md
mkdir src
cp ~/some/file.c src/file.c
```

The exact command names may vary, but the functionality shall exist.

### 7.2 View Isolation

When a view is mounted, directory listings shall show only directories and files mapped into that view.

A file not present in the active view shall not be visible via `readdir`.

A file not present in the active view shall not be openable by guessing its backing-store path through the mounted view.

A path traversal attempt using `..` shall not escape the mounted view.

### 7.3 Normal File Access

Existing programs shall be able to use the mounted view with ordinary file operations:

1. `open`
2. `read`
3. `write`
4. `close`
5. `stat`
6. `rename`
7. `unlink`
8. `mkdir`
9. `rmdir`
10. `truncate`
11. `fsync`, at least as a best-effort pass-through operation

The prototype shall expose behavior close enough to a normal POSIX filesystem for basic command-line tools to work.

### 7.4 Multiple Views

The system shall support multiple named views.

Different views may be mounted at different mount points at the same time.

Example command shape:

```sh
viewfs mount programming ~/mnt/programming
viewfs mount writing ~/mnt/writing
```

The same object may appear in both views.

Changes to the content of a shared object through one view shall be visible through another view after ordinary filesystem cache effects are resolved.

### 7.5 Per-View Naming

The same object may appear under different paths in different views.

Example:

```text
programming:/projects/tool/README.md
writing:/articles/tool-notes.md
```

Both paths may refer to the same underlying object.

Changing file content through either path shall change the same object.

Renaming a file in one view shall change that view's path mapping only unless the implementation explicitly performs an object-level operation requested by the administration tool.

### 7.6 Adding Files Through a View

When a user creates a new file inside a mounted view, the prototype shall:

1. Create a new object.
2. Store the file contents in the backing store.
3. Add a path mapping for the new object in the active view.
4. Persist the mapping.
5. Ensure the new file is visible after unmount and remount of the same view.

The new object shall not automatically appear in unrelated views unless the user or view rules require it.

### 7.7 Removing Files Through a View

When a user deletes a file path from a mounted view, the default behavior shall remove that path mapping from the active view.

If the object appears in other views, the object and its content shall remain available through those other views.

If the removed path was the object's last view mapping, the prototype shall either:

1. Mark the object as orphaned but preserve its content, or
2. Delete the object content only if an explicit configured policy says to do so.

For the initial prototype, the safer default shall be to preserve orphaned content.

### 7.8 Renaming and Moving Files Within a View

Renaming or moving a file inside a view shall update that view's path mapping.

The object identity shall remain the same.

Other views' mappings shall not be changed by default.

### 7.9 Directories

Directories in a view may be:

1. Explicitly persisted as directory mappings, or
2. Implied by file path mappings.

The behavior shall be consistent and documented in the implementation README.

Directory creation through `mkdir` shall persist in the active view.

Directory removal through `rmdir` shall succeed only when the directory is empty in that view.

### 7.10 Attributes and Tags

The prototype shall allow arbitrary attributes or tags to be associated with objects.

Minimum required operations:

1. Set an attribute.
2. Get attributes for an object.
3. Remove an attribute.
4. List objects matching an attribute.
5. Add a tag.
6. Remove a tag.
7. List tags for an object.

Example command shapes:

```sh
viewfs tag add OBJECT_ID project:compiler
viewfs tag remove OBJECT_ID project:compiler
viewfs attr set OBJECT_ID language C
viewfs attr get OBJECT_ID
viewfs find --tag project:compiler
viewfs find --attr language=C
```

The exact command syntax may vary.

### 7.11 View Membership Management

The prototype shall provide a way to add or remove an object from a view.

Example command shapes:

```sh
viewfs view add programming OBJECT_ID /src/main.c
viewfs view remove programming /src/main.c
```

When adding an object to a view, the user shall be able to choose the view path.

When removing an object from a view, only the selected view mapping shall be removed by default.

### 7.12 View Listing and Inspection

The administration tool shall support:

1. Listing all views.
2. Creating a view.
3. Deleting a view.
4. Showing a view's mappings.
5. Showing all paths for a given object across views.
6. Showing object metadata.

Example command shapes:

```sh
viewfs view list
viewfs view create programming
viewfs view delete programming
viewfs view show programming
viewfs object show OBJECT_ID
viewfs object paths OBJECT_ID
```

## 8. View Definition Requirements

### 8.1 Explicit Mappings

The prototype shall support explicit path mappings from a view path to an object.

This is the minimum required view definition mechanism.

A mapping shall include:

1. View name.
2. View path.
3. Object ID.
4. File type, if needed.
5. Timestamp metadata sufficient for display and synchronization.

### 8.2 Attribute-Based Membership

The prototype shall include at least a minimal mechanism for selecting or listing objects by attributes or tags.

For the initial implementation, attribute-based membership may be implemented as an administration-tool operation that materializes explicit mappings into a view.

Example:

```sh
viewfs view populate programming --tag project:compiler --under /project
```

This command may create explicit mappings for all matching objects.

The prototype does not need to implement a complex live query language for view membership unless doing so is simpler than materialized mappings.

### 8.3 Future Rule Language Placeholder

The implementation shall leave room for future declarative view rules, but the initial prototype shall not depend on a sophisticated rule language.

The README shall state whether view membership is:

1. Fully explicit,
2. Materialized from attributes,
3. Dynamically queried from attributes, or
4. A combination.

## 9. Object Identity Requirements

Each object shall have a stable internal identifier.

Object identity shall not depend on:

1. Backing-store path.
2. View name.
3. View path.
4. Current filename.
5. Current parent directory.

The prototype shall be able to report an object's ID through the administration tool.

It is acceptable for the FUSE mount to expose ordinary inode numbers that are derived from object IDs, provided the behavior is stable enough for normal tools.

## 10. Storage Requirements

### 10.1 Backing Store Layout

The implementation shall maintain a backing store directory containing:

1. File contents.
2. Object metadata.
3. View definitions.
4. View path mappings.
5. Attribute and tag metadata.

The backing store location shall be configurable.

Example command shape:

```sh
viewfs init ~/.local/share/viewfs
```

### 10.2 Metadata Persistence

Metadata shall survive:

1. Unmounting a view.
2. Remounting a view.
3. Restarting the FUSE daemon.
4. Rebooting the system, assuming the backing store is persistent.

### 10.3 Metadata Format

The prototype may use any simple local metadata store, such as:

1. SQLite,
2. JSON files,
3. TOML/YAML files,
4. A small embedded database.

The chosen format shall be documented.

The implementation shall avoid requiring an external database server.

### 10.4 Content Storage

File contents shall be stored under the backing store.

The implementation shall avoid exposing backing-store internals through mounted views.

A reasonable prototype approach is content files named by object ID.

## 11. Security Requirements

### 11.1 Mounted View Boundary

The FUSE filesystem shall not expose objects outside the active view through normal mounted-path operations.

The implementation shall defend against:

1. `..` traversal escaping the view.
2. Symlink traversal escaping the view when symlink support is implemented.
3. Absolute backing-store paths being interpreted as view paths.
4. Path normalization bugs.
5. Opening unmapped objects through guessed names.

### 11.2 Backing Store Protection

The README shall warn that users must not give sandboxed processes direct access to the backing store if they want view isolation.

The prototype shall assume that the backing store is trusted and controlled by the user running the FUSE daemon.

### 11.3 Process-Level Isolation Limits

The prototype shall clearly document that FUSE view isolation applies to accesses through the FUSE mount only.

For stronger process isolation, the README shall recommend combining the mounted view with existing Linux mechanisms such as:

1. Mount namespaces,
2. Containers,
3. `chroot` where appropriate,
4. Bubblewrap or similar user-space sandboxing tools,
5. SELinux/AppArmor if later integrated.

The implementation itself does not need to configure these mechanisms.

### 11.4 Permissions

The prototype shall support basic POSIX mode bits for files and directories.

Minimum behavior:

1. Store mode bits for objects or mappings.
2. Return mode bits through `getattr`.
3. Respect read-only versus writable files where practical.
4. Preserve executable bits.

It is acceptable for ownership to be simplified to the user running the FUSE daemon in the initial prototype.

## 12. FUSE Operation Requirements

The prototype shall implement enough FUSE callbacks to support common shell and editor usage.

### 12.1 Required Operations

The implementation shall support:

1. `getattr`
2. `readdir`
3. `open`
4. `read`
5. `write`
6. `create`
7. `mkdir`
8. `unlink`
9. `rmdir`
10. `rename`
11. `truncate`
12. `utimens`
13. `flush` or equivalent
14. `fsync` or documented best-effort behavior
15. `release`

### 12.2 Recommended Operations

The implementation should support:

1. `statfs`
2. `chmod`
3. `chown`, even if simplified
4. `readlink`
5. `symlink`
6. Extended attributes, if convenient
7. `access`

### 12.3 Path Resolution

All FUSE path resolution shall be relative to the active view.

The implementation shall canonicalize paths before lookup.

Canonicalization shall reject or normalize:

1. Duplicate slashes.
2. `.` components.
3. `..` components.
4. Empty path components except root.
5. Invalid or unsupported filenames.

### 12.4 Error Behavior

The filesystem shall return normal POSIX-style errors.

Examples:

1. `ENOENT` for paths not visible in the active view.
2. `EEXIST` when creating a mapping that already exists.
3. `ENOTDIR` when a path component is not a directory.
4. `EISDIR` when reading a directory as a file.
5. `ENOTEMPTY` when removing a non-empty directory.
6. `EACCES` for permission failures.
7. `EROFS` if mounted read-only and a write is attempted.

## 13. Command-Line Tool Requirements

The project shall provide a command-line tool, named `viewfs` unless another name is chosen consistently.

The tool shall support at least the following conceptual operations.

### 13.1 Repository Commands

```sh
viewfs init STORE_PATH
viewfs status
```

### 13.2 View Commands

```sh
viewfs view create VIEW_NAME
viewfs view list
viewfs view delete VIEW_NAME
viewfs view show VIEW_NAME
```

### 13.3 Mount Commands

```sh
viewfs mount VIEW_NAME MOUNTPOINT
viewfs unmount MOUNTPOINT
```

The unmount command may wrap `fusermount3 -u`.

### 13.4 Object Commands

```sh
viewfs object import SOURCE_PATH
viewfs object show OBJECT_ID
viewfs object paths OBJECT_ID
viewfs object list
```

Importing a file shall create an object in the backing store.

### 13.5 Mapping Commands

```sh
viewfs view add VIEW_NAME OBJECT_ID VIEW_PATH
viewfs view remove VIEW_NAME VIEW_PATH
```

### 13.6 Attribute Commands

```sh
viewfs attr set OBJECT_ID KEY VALUE
viewfs attr get OBJECT_ID
viewfs attr remove OBJECT_ID KEY
```

### 13.7 Tag Commands

```sh
viewfs tag add OBJECT_ID TAG
viewfs tag remove OBJECT_ID TAG
viewfs tag list OBJECT_ID
viewfs find --tag TAG
viewfs find --attr KEY=VALUE
```

## 14. Data Model Requirements

The implementation shall maintain equivalent information to the following conceptual entities.

### 14.1 Object Record

Each object record shall contain:

1. Object ID.
2. File type.
3. Content storage reference.
4. Size.
5. Created timestamp.
6. Modified timestamp.
7. Mode bits.
8. Optional owner/group fields.
9. Optional checksum field.

### 14.2 View Record

Each view record shall contain:

1. View ID or name.
2. Created timestamp.
3. Modified timestamp.
4. Optional description.

### 14.3 Mapping Record

Each mapping record shall contain:

1. View name or ID.
2. Normalized view path.
3. Object ID, for files.
4. Directory marker, for explicit directories if used.
5. Per-view display name implied by path.
6. Created timestamp.
7. Modified timestamp.

### 14.4 Attribute Record

Each attribute record shall contain:

1. Object ID.
2. Key.
3. Value.
4. Created timestamp.
5. Modified timestamp.

### 14.5 Tag Record

Each tag record shall contain:

1. Object ID.
2. Tag string.
3. Created timestamp.

## 15. Consistency Requirements

### 15.1 Shared Object Content

If the same object appears in multiple views, all view paths shall reference the same content.

Writing through one view path shall update the object content.

Other views shall observe the updated content.

### 15.2 Independent View Paths

View path mappings shall be independent by default.

Renaming or moving a mapping in one view shall not rename or move mappings in other views.

Deleting a mapping in one view shall not remove mappings from other views.

### 15.3 Orphaned Objects

The implementation shall provide a way to list objects with no view mappings.

Example command shape:

```sh
viewfs object list --orphaned
```

The implementation should provide an explicit command to delete orphaned objects.

Example command shape:

```sh
viewfs object delete OBJECT_ID
```

Deletion of object content shall be explicit.

## 16. Logging and Diagnostics

The prototype shall provide useful diagnostics for development.

Requirements:

1. A verbose mode for the FUSE daemon.
2. Clear error messages from the CLI.
3. A way to inspect the backing store status.
4. A way to check metadata consistency.
5. Logs sufficient to debug path resolution problems.

Example command shape:

```sh
viewfs check
```

## 17. Testing Requirements

The project shall include automated tests.

At minimum, tests shall cover:

1. Initializing a backing store.
2. Creating views.
3. Importing objects.
4. Adding an object to one view.
5. Adding the same object to two views.
6. Assigning different paths to the same object in different views.
7. Mounting a view.
8. Listing visible files.
9. Confirming invisible files are not listed.
10. Reading a visible file.
11. Attempting to read a non-visible file and receiving `ENOENT`.
12. Writing a shared object through one view and reading the change through another view.
13. Renaming a file in one view without affecting another view.
14. Removing a file from one view without deleting the object from another view.
15. Persisting view mappings across unmount and remount.
16. Setting and retrieving attributes.
17. Setting and retrieving tags.
18. Finding objects by tag.
19. Finding objects by attribute.
20. Handling `..` traversal safely.

Tests may use temporary directories and temporary FUSE mount points.

## 18. Demonstration Scenario

The completed prototype shall include a scripted demonstration.

The demonstration shall:

1. Initialize a new backing store.
2. Create at least two views:
   - `programming`
   - `writing`
3. Import at least three files.
4. Add one file only to `programming`.
5. Add one file only to `writing`.
6. Add one shared file to both views under different paths.
7. Mount both views simultaneously at separate mount points.
8. Show that each view lists only its mapped files.
9. Modify the shared file in one view.
10. Show the modification through the other view.
11. Rename the shared file in one view.
12. Show that the other view's path is unchanged.
13. Unmount and remount.
14. Show that mappings persist.

## 19. Build and Platform Requirements

The prototype shall target Linux.

The implementation shall support FUSE 3.

The build instructions shall include Fedora-oriented setup commands.

Expected Fedora dependencies include:

```sh
sudo dnf install fuse3 fuse3-devel gcc make pkgconf-pkg-config
```

If the implementation uses another language, the README shall include equivalent dependency installation instructions.

The project shall include a simple build command, such as one of:

```sh
make
cargo build
go build
python -m build
```

The selected implementation language is not specified by this document.

## 20. Documentation Requirements

The project shall include a README explaining:

1. What the prototype demonstrates.
2. What it does not attempt to solve.
3. How to build it.
4. How to initialize a backing store.
5. How to create views.
6. How to import files.
7. How to map files into views.
8. How to mount and unmount views.
9. How to use tags and attributes.
10. How shared objects behave.
11. What the security limitations are.
12. How to run tests.
13. How to run the demonstration script.

The README shall explicitly state that the prototype is experimental and should not be used for irreplaceable data.

## 21. Acceptance Criteria

The implementation is acceptable when all of the following are true:

1. A user can initialize a backing store.
2. A user can create at least two views.
3. A user can import files as objects.
4. A user can map objects into views at arbitrary paths.
5. A user can mount a view with FUSE.
6. The mounted view behaves like a normal directory tree for basic shell commands.
7. Files outside the active view are not visible through that mount.
8. The same object can be visible in multiple views.
9. The same object can have different names or paths in different views.
10. Content changes through one view are reflected through other views referencing the same object.
11. Renaming a mapping in one view does not rename the mapping in another view.
12. Removing a mapping in one view does not delete the object from other views.
13. View mappings persist after unmount and remount.
14. Tags and attributes can be assigned and queried.
15. Automated tests cover the required behavior.
16. The README documents build, usage, limitations, and demo steps.

## 22. Suggested Repository Layout

The implementation may use any suitable layout, but the repository should contain equivalent components:

```text
viewfs/
  README.md
  SPEC.md
  Makefile or build file
  src/
    fuse_daemon.*
    cli.*
    metadata_store.*
    path_resolution.*
    object_store.*
  tests/
    test_views.*
    test_mappings.*
    test_tags_attrs.*
    test_fuse_mount.*
  examples/
    demo.sh
```

This layout is only a suggestion for organizing the work. The requirements in this specification are authoritative over the suggested layout.

## 23. Important Design Constraints

The implementation shall preserve these core conceptual constraints:

1. Views are first-class named entities.
2. A view is not merely a directory; it is a separate user-facing hierarchy.
3. Object identity is independent of any view path.
4. View paths are mappings from a view hierarchy to objects.
5. The same object may appear in multiple views.
6. The same object may appear under different names in different views.
7. Programs access files normally through paths.
8. Programs using a mounted view shall see only that view's visible files.
9. Attributes and tags are associated with objects, not merely paths.
10. The prototype is layered over an existing filesystem through FUSE.

## 24. Handoff Instruction for Claude Code

Implement the project described in this specification as a working Linux FUSE 3 prototype.

Favor a small, understandable, testable implementation over a complex or highly optimized one.

Do not attempt to implement kernel code.

Do not attempt to design a production filesystem.

Do not add major unrelated features.

Where this specification leaves implementation choices open, choose the simplest option that satisfies the acceptance criteria and document the choice in the README.
