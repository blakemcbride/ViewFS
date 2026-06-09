# ViewFS Rewrite Plan — Property-Driven View Membership

Companion to `Design.md` / `Plan.md`. This document specifies the rewrite
that **replaces the explicit-mapping membership model** with a
**dynamic, property-driven model**. Where this document and the original
`Design.md` / `Plan.md` conflict, **this document wins** for the rewrite.

The build infrastructure (Makefile, libpq/libfuse stack, object store
layout, canonicalizer, object-id generator, checksum machinery, the
LISTEN/NOTIFY plumbing, the integration harness) is **kept as-is**. The
rewrite is concentrated in the schema, path-resolution, the FUSE
membership/readdir/create/rename logic, and the CLI surface for
directory properties.

---

## 1. Locked design decisions

These come from the design brief and the follow-up clarifications. They
are not open for re-litigation inside the phases; they are the contract
the phases implement.

1. **Property model fully replaces explicit mappings.** The `mappings`
   table and every explicit per-file placement go away. A directory's
   file contents are *computed* from properties, never stored.
2. **Directories are explicit; files are implied.** Directories form a
   per-view tree with a single root, created via `mkdir` through the
   mount (or a CLI command). Files are never directory rows — they
   appear by matching.
3. **Each directory carries a set of property/value pairs**, managed by a
   CLI utility (add / list / delete / change). A freshly `mkdir`'d
   directory has an **empty** property set.
4. **Matching is superset:** a file (object) appears in directory `D`
   iff the object's property set is a **superset** of `D`'s *effective*
   property set (defined in §3.2). The empty set is a subset of
   everything, so an empty-prop directory matches **every** object.
5. **Properties are multi-valued.** Both objects and directories may
   carry several values for the same key (`author=blake`,
   `author=jane`). A directory pair `author=blake` requires that the
   object have *that specific* `(author, blake)` pair among its values.
   When a directory lists multiple values for one key, the object must
   have **all** of them (AND semantics across every directory pair).
6. **Per-property flow flag, default off.** Each directory property pair
   carries a boolean `flow`. With `flow=false` (default) the pair
   constrains only that directory ("independent"). With `flow=true` the
   pair also applies to all descendant directories ("cumulative" for
   that pair). A directory's effective set is its own pairs plus every
   ancestor pair marked `flow=true` (§3.2).
7. **Full phased rewrite**, not an incremental bolt-on.

### 1.1 Decisions made here, flagged for confirmation

These follow necessarily from the model but were not explicitly chosen by
the user; each is the recommended default and is called out so it can be
overridden before Phase 1 starts.

- **D1 — Filenames are an object attribute.** Because a directory is a
  query, every matching object needs a display name. The object gains an
  intrinsic `name` (basename on import; settable). The same object shows
  under the same name in every directory it matches. (Old model allowed a
  different name per placement; the new model has one name per object.)
- **D2 — Name collisions within one directory are disambiguated by a
  short object-id suffix.** *(Implemented in R5.)* When two matching objects
  share a `name` in the same directory (or a member clashes with a child
  directory's name), `readdir` shows each as `report.txt~<idprefix>` where
  the prefix is the shortest length that distinguishes the colliding group
  (minimum 4 hex). A uniquely-named member is shown bare. `lookup`/`getattr`
  resolve the bare form first, then fall back to parsing `base~hexprefix`.
- **D3 — `unlink` deletes the object.** `rm <file>` removes the object
  row and its content blob outright (it vanishes from every view/directory
  it matched). To remove a file from one directory *without* destroying
  it, the user changes the object's properties instead (CLI `prop unset`,
  or `mv` to a directory whose pairs it no longer matches). This is the
  simple, POSIX-like behavior the user chose over property-stripping.
- **D4 — Through-the-mount `cp` creates an object with only the
  destination directory's effective props.** A userspace `cp` opens the
  source for read and creates a fresh file at the destination; FUSE sees
  only the new bytes, not the source object's identity, so the "retain
  the source object's other props" clause of the brief cannot be honored
  from `create`+`write` alone. The richer copy (duplicate content, drop
  source-dir pairs, gain dest-dir pairs, **retain all other props**) is
  provided as an object-level CLI command `viewfs object copy`. `mv`
  through the mount *does* get full move semantics (§5, `op_rename`)
  because FUSE hands us both endpoints of the same object.

If any of D1–D4 is wrong, say so before Phase 1 — they shape the schema.

---

## 2. Conceptual model (the one-paragraph version)

A **view** is a named, per-view tree of **directories**, each with an
attached multiset of `(key, value, flow)` property pairs. An **object**
is a content blob with its own multiset of `(key, value)` properties and
an intrinsic `name`. The files listed in directory `D` of view `V` are
exactly the objects whose property multiset is a superset of `D`'s
*effective* property set (D's own pairs ∪ ancestor pairs flagged
`flow`). Nothing records "object X is in directory D" — it is always
recomputed. Mutating where a file appears means mutating the object's
properties (move) or the directory's properties (re-filter).

---

## 3. Schema (rewritten `0001_init.sql`, schema version 1)

**No pre-existing store uses the old model**, so there is nothing to
migrate and no drop-and-recreate dance. The canonical `0001_init.sql`
(and its embedded copy in `store.c`) is **rewritten in place** to the
schema below; `0002_checksum_state.sql` is folded into it (the
`checksum_state` column moves into the `objects` definition).
`VIEWFS_SCHEMA_VERSION` resets to `1`. The old `mappings`/`attributes`/
`tags` definitions simply cease to exist in the migration source.

```sql
-- objects: schema-v2 columns (object_id, kind, size, mode, uid, gid,
-- *_ns, checksum, checksum_state, source_path, symlink_target) PLUS an
-- intrinsic display name. checksum_state is inlined here (was 0002).
-- ... objects table as before, with one added column:
    name           TEXT NOT NULL DEFAULT '',

-- object_props REPLACES both `attributes` and `tags`.
-- Multi-valued: PK includes value. A "tag" is sugar for key='tag'.
CREATE TABLE object_props (
    object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,
    key       TEXT NOT NULL,
    value     TEXT NOT NULL,          -- '' allowed (valueless property/tag)
    ctime_ns  BIGINT NOT NULL,
    mtime_ns  BIGINT NOT NULL,
    PRIMARY KEY (object_id, key, value)
);
-- The membership-critical index: (key,value) -> object.
CREATE INDEX object_props_kv ON object_props (key, value);

-- view_dirs REPLACES `mappings`. Directories ONLY; files are never rows.
CREATE TABLE view_dirs (
    view_name   TEXT NOT NULL REFERENCES views(view_name) ON DELETE CASCADE,
    dir_path    TEXT NOT NULL,        -- canonical, leading '/'
    parent_path TEXT NOT NULL,        -- '' for the root's children; root itself is '/'
    name        TEXT NOT NULL,        -- final component; '' for root
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

Every view gets a root row `('/', '', '', 0755, …)` created in
`vfs_view_create` so the root always resolves and `ls /mnt` works on a
brand-new view (it shows every object, since root's effective set is
empty — see §3.2).

### 3.1 What is gone

`mappings` (entire concept), `attributes` (→ `object_props`), `tags`
(→ `object_props` with `key='tag'`), `mappings.mode_override`,
`mappings.entry_kind`. All `vfs_mapping_*` API entry points
(`include/viewfs/viewfs.h` lines ~297–339) are deleted.

### 3.2 Effective property set of a directory

```
effective(V, D) =
      { (k,v) | (V,D,k,v,_)            ∈ view_dir_props }          -- own pairs
    ∪ { (k,v) | (V,A,k,v,flow=true)    ∈ view_dir_props,
                A is a strict ancestor of D in view_dirs }          -- flowed pairs
```

Ancestors of `D` are found by walking `parent_path` up to `/`, or in one
query by `dir_path` being a prefix of `D` on a path boundary. For the
flowed set we only need ancestor rows where `flow=true`
(`view_dir_props_flow` index).

### 3.3 The membership query (relational division)

Files in directory `D` of view `V` = objects whose props ⊇ `effective(V,D)`.
Let `E` be that effective set with cardinality `n`:

```sql
-- n = |E|; pairs bound as a VALUES list or array params.
SELECT op.object_id
  FROM object_props op
  JOIN (VALUES (k1,v1), (k2,v2), …, (kn,vn)) AS need(key,value)
    ON op.key = need.key AND op.value = need.value
 GROUP BY op.object_id
HAVING count(*) = n;          -- has every required pair
```

`n = 0` (empty effective set) short-circuits to "all objects"
(`SELECT object_id FROM objects`). This is the single hot query of the
whole system; the `object_props_kv` index makes the join an index scan.

---

## 4. Path resolution and naming

`lookup(V, path)` / `getattr`:

1. Canonicalize `path` (unchanged `vfs_path_canonicalize`).
2. If `(V, path)` is a row in `view_dirs` → it is a **directory**;
   return dir attrs (mode, ctimes from the row).
3. Otherwise split into `parent = dirname(path)`, `base = basename(path)`.
   `parent` must be a directory in `view_dirs` (else `ENOENT`). Compute
   the membership set of `parent`; find the object(s) whose `name`
   matches `base` (or whose `name~<idprefix>` form matches per **D2**).
   Exactly one → **file**; return its object attrs. Zero → `ENOENT`.

`readdir(V, D)`:

- Synthetic `.` and `..`.
- **Subdirectories:** `SELECT name FROM view_dirs WHERE view_name=$V AND parent_path=$D`.
- **Files:** run §3.3 against `effective(V,D)`, list each object by `name`,
  applying D2 disambiguation when two listed objects share a name.

A name that collides with an existing subdirectory name in the same
directory is shown with the D2 suffix on the *file*, since the directory
owns the bare name.

---

## 5. FUSE callback semantics (the rewrite surface in `src/fuse/ops.c`)

`ops.c` is ~1500 lines and 27 callbacks. Read-path callbacks
(`op_open`, `op_read`, `op_release`, `op_flush`, `op_fsync`, `op_statfs`,
`op_access`) and the checksum machinery are **unchanged** once a path
resolves to an object. The callbacks that change:

| Callback | New behavior |
|---|---|
| `op_getattr` | §4 resolution (dir row vs. computed file). |
| `op_readdir` | §4 listing (subdir rows + membership query). |
| `op_lookup`/lookup-via-getattr | §4. Honors D2 suffix forms. |
| `op_mkdir` | INSERT a `view_dirs` row with empty property set, auto-creating missing parents. No object involved. |
| `op_rmdir` | `ENOTEMPTY` if it has child dirs **or** a non-empty membership set; else delete the `view_dirs` row (cascades its `view_dir_props`). |
| `op_create` | New object: generate id, empty content, `name = base`, then **assign `effective(V,parent)` as the object's props** (the "files created in a directory take on its pairs" rule). For a flowed pair the object simply gets the `(k,v)`; flow is a directory attribute, not an object one. |
| `op_write`/`op_truncate` | Unchanged (operate on the resolved object's content + checksum stream). |
| `op_unlink` | **D3:** delete the object row (cascades `object_props`) and its content blob. To de-list without deleting, change props instead. |
| `op_rename` (file, same view) | **Move:** the object's props become `(props − effective(V,src_parent)) + effective(V,dst_parent)`; `name` becomes the new basename. Content untouched. |
| `op_rename` (dir, same view) | Re-parent the `view_dirs` subtree (the existing prefix-rewrite UPDATE pattern), carrying `view_dir_props`. Recompute is implicit (effective sets are always derived). |
| `op_rename` cross-view / `EXCHANGE` | `EXDEV` / `EINVAL` as today. |
| `op_chmod` | Directory → update `view_dirs.mode`. File → `objects.mode`. (No more `mode_override`.) |
| `op_symlink`/`op_readlink` | Symlink object (`kind='symlink'`) created in a directory gets that directory's effective props, exactly like `op_create`. |
| `op_*xattr` | Re-pointed from `attributes` to `object_props`. `user.viewfs.<key>` xattrs round-trip as `(key,value)` props. Multi-value means `getxattr` returns the lexically-first value (or a documented join); `setxattr` replaces all values for the key. |
| `op_chown`, `op_utimens` | Unchanged. |

NOTIFY payloads (`<view>\t<parent_path>`) are unchanged; mutations that
change membership (`op_create`, `op_unlink`, `op_rename`, and **any CLI
property change**) emit a NOTIFY for the affected directory and root so a
mounted view re-queries. Because membership is global to a view, a
property change with no obvious "parent" emits a view-wide invalidation
(payload `<view>\t*`, with the listener invalidating the whole tree).

---

## 6. CLI surface

Removed: `viewfs view add`, `viewfs view remove`, `viewfs view populate`,
`viewfs attr *`, `viewfs tag *` (the old explicit-mapping and
informational-attribute commands).

Added / changed:

```
# Properties (multi-valued). Replaces `attr` and `tag`. ONE command for
# both a file's properties and a directory's filter; the TARGET decides
# which. TARGET = object ID|PREFIX, VIEW:DIR, or a path/name in a mount
# (omitted = the current directory). --flow/--effective: directory targets.
viewfs prop set    TARGET KEY VALUE [--flow]    # add/set a pair
viewfs prop unset  TARGET KEY [VALUE]           # remove one value, or all of KEY
viewfs prop list   [TARGET] [--effective]

viewfs object name  ID|PREFIX [NEWNAME]         # get/set the intrinsic name
viewfs object copy  ID|PREFIX VIEW:DIR          # D4 full copy: dup content,
                                                # gain DIR's effective pairs, keep the rest

# Directory tree (filters are managed with `prop` above).
viewfs dir mkdir   [VIEW] DIR                    # also creatable via mount mkdir
viewfs dir rmdir   [VIEW] DIR
viewfs dir ls      [[VIEW] DIR]                  # show computed contents

# Find, re-expressed over properties (multi-value aware).
viewfs find --prop KEY[=VALUE] [--prop KEY[=VALUE] ...]   # AND across pairs
```

> **CLI evolution note.** An earlier iteration exposed directory filters via
> a separate `viewfs dir prop …` subcommand. That was folded into `viewfs
> prop` (targets distinguish a file from a directory), and `prop`/`dir`
> commands gained cwd/path inference inside a mount. The text above reflects
> the current surface.

`viewfs object import HOST_PATH [--into VIEW:DIR]` keeps its shape, but
`--into VIEW:DIR` now means "assign `effective(V,DIR)`'s pairs to the
imported object" (so it appears in `DIR`) instead of inserting a mapping
row. `--name` overrides the derived basename.

`viewfs check` and `viewfs status` lose their `mappings` queries
(`cmd_check.c`, `cmd_status.c`) and gain:
- orphan objects = objects matching no directory in any view (was: zero
  mappings);
- directory-tree integrity = every `view_dirs` row reachable from its
  view root; no `view_dir_props` row without its `view_dirs` parent (FK
  guarantees, scanned anyway);
- `object_props` ↔ `objects` referential sanity.

---

## 7. libviewfs API changes (`include/viewfs/viewfs.h`)

- **Delete** the entire Mappings section (`vfs_mapping_*`), the Attributes
  section (`vfs_attr_*`), and the Tags section (`vfs_tag_*`).
- **Add** `vfs_object_props_*` (set/unset/list, multi-value), and
  `vfs_object_set_name`.
- **Add** a Directories section: `vfs_dir_create`, `vfs_dir_remove`,
  `vfs_dir_list_children`, `vfs_dir_prop_add/set/delete/list`,
  `vfs_dir_effective_props` (returns the computed set used by the daemon
  and by `import --into`).
- **Add** the membership primitive
  `vfs_dir_members(s, view, dir_path, vfs_object_cb, ud)` implementing
  §3.3 — the single function the FUSE `readdir` and CLI `dir ls` share.
- **Add** `vfs_lookup(s, view, path, …)` returning {is_dir, object} per §4.
- Replace `vfs_find_by_tag`/`vfs_find_by_attr` with
  `vfs_find_by_props(s, pairs[], n, cb, ud)`.
- Rename the check helpers: `vfs_check_mappings_*` →
  `vfs_check_dirs_reachable`, `vfs_check_props_orphans`.

The store lifecycle, content-blob, object-CRUD, checksum, and
canonicalizer sections of the header are untouched.

---

## 8. Files touched (rewrite map)

| File | Action |
|---|---|
| `src/libviewfs/migrations/0001_init.sql` | **rewritten in place** — §3 (folds in old `0002`) |
| `src/libviewfs/migrations/0002_checksum_state.sql` | **delete** — `checksum_state` now lives in `0001` |
| `src/libviewfs/store.c` | `VIEWFS_SCHEMA_VERSION` stays/ resets to 1; update embedded `MIGRATION_0001` literal to match the rewritten SQL |
| `src/libviewfs/mappings.c` | **delete**, replace with `view_dirs.c` (dir tree + dir props + effective set) |
| `src/libviewfs/attrs.c`, `tags.c`, `find.c` | **delete/merge** into `object_props.c` + `members.c` |
| `src/libviewfs/objects.c` | add `name` column handling; orphan = "matches no dir" |
| `include/viewfs/viewfs.h` | §7 |
| `src/fuse/ops.c` | §5 — membership-aware getattr/readdir/lookup; new create/unlink/rename/mkdir/rmdir; xattr re-point |
| `src/cli/cmd_view.c` | strip `add/remove/populate`; add `dir` subtree |
| `src/cli/cmd_attr.c`, `cmd_tag.c` | **delete**, replace with `cmd_prop.c` |
| `src/cli/cmd_object.c` | add `name`, `copy`; `import --into` semantics |
| `src/cli/cmd_find.c` | `--prop` (multi, AND) |
| `src/cli/cmd_check.c`, `cmd_status.c` | re-point to new tables/invariants |
| `src/cli/main.c`, `common.c` | dispatcher wiring for new subcommands |

---

## 9. Phased plan

Each phase builds and passes `make` clean (`-Wall -Wextra -Wpedantic`)
and its own tests before the next begins.

### Phase R0 — Schema & libviewfs core (no FUSE)
- Rewrite `0001_init.sql` (§3, folding in `0002`); delete `0002`; update
  the embedded `MIGRATION_0001` literal in `store.c`.
- Implement `object_props.c`, `view_dirs.c` (incl. `effective` walk),
  `members.c` (§3.3), `objects.c` `name` support.
- Rewrite the header (§7). Delete dead mapping/attr/tag API.
- **Exit:** unit tests for `effective()` (independent vs. flowed),
  superset matching (incl. multi-value AND, empty-set = all), and
  reachability. Build green; CLI not yet rewired.

### Phase R1 — CLI rewrite
- `cmd_prop.c`, `cmd_view.c` `dir` subtree, `object name|copy`,
  `import --into`, `find --prop`, `check`/`status` re-point.
- **Exit:** a shell session can `init`, create a view, `dir mkdir`,
  attach dir props (with/without `--flow`), import objects with props,
  and see correct membership via `viewfs dir ls` — including a flowed
  pair visible to a child dir and an independent pair that is not.

### Phase R2 — FUSE read path over membership
- Rewrite `op_getattr`, `op_readdir`, lookup (§4) and `op_open`/`op_read`
  resolution. D2 name disambiguation.
- **Exit:** mount a view read-only; `ls`/`cat` reflect property-driven
  membership; a CLI prop change becomes visible (NOTIFY) within the cache
  window. Reuse/extend `test_mount_ls`, `test_read`, `test_traversal`.

### Phase R3 — FUSE write path
- `op_mkdir`/`op_rmdir` (dir rows), `op_create`/`op_symlink` (assign
  effective props), `op_unlink` (D3 strip), `op_rename` move (file) and
  re-parent (dir), `op_chmod`, xattr re-point.
- **Exit:** `mkdir`, `cp` into a dir (object gains the dir's pairs and
  appears), `mv` between dirs (object's pairs swap, content stable),
  `rm` (object leaves the dir, survives as orphan). Rewrite
  `test_import`, `test_cross_view`, `test_sharing`, `test_rename`,
  `test_unlink`, `test_attrs` around the new semantics.

### Phase R4 — NOTIFY for property changes & docs
- View-wide invalidation on CLI/daemon property mutations (§5).
- Update `Design.md`, `README.md`, `RUNNING.md`, `examples/demo.sh`, and
  `viewfs_fuse_prototype_spec.md` to the property model.
- **Exit:** full integration suite green; demo shows the same object
  appearing in two directories by property match, a flowed pair
  cascading to a child, and `mv` re-homing a file by property edit.

### Phase R5 — `check`/diagnostics polish & crash test  *(done)*
- `viewfs check` invariants for the property model: propertyless objects
  (informational), directories with a missing parent, directories with an
  inconsistent `name`/`parent_path` (`vfs_check_dir_structure`), and orphan
  `object_props` rows; content↔object cross-check and checksum coverage
  retained.
- **D2 name-collision disambiguation wired** into the daemon `readdir`
  (display) and path resolution (lookup) — see §1.1 D2.
- Integration: `test_check` (corruption is detected, non-zero exit) and
  `test_dupnames` (same-named objects are listed + addressable); `test_crash`
  still green — durability semantics are unchanged because the content path
  and checksum machinery are untouched.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| Membership query cost on large stores (relational division per readdir) | `object_props_kv` index + `HAVING count(*)=n` plan; cache effective sets per directory in the daemon, invalidated by NOTIFY. |
| Empty-prop directory dumps the entire object table into one `ls` | Documented and intentional (root behaves this way). `viewfs dir ls` warns when `n=0`. |
| Name collisions across many matching objects | D2 suffixing; `viewfs object name` to rename; collisions logged under `--verbose`. |
| Through-the-mount `cp` cannot retain source object's extra props | D4: `viewfs object copy` is the lossless path; README documents the mount `cp` limitation. |
| Flow semantics confusing (which pairs cascade) | `viewfs dir prop list --effective` shows the resolved set with the source directory of each flowed pair. |
| `mv` causing a file to also (dis)appear from unrelated directories | Inherent to a global-by-property model; documented. The set that changes is exactly the objects matching the swapped pairs. |

---

## 11. Open items to confirm before Phase R0

- **D1–D4** in §1.1 (intrinsic name, collision suffix, unlink-strips,
  cp-creates-fresh / `object copy` for lossless).
- Whether `find`/`prop` should treat a valueless tag as `key=''` or as a
  distinct namespace (`tag:` prefix). Current plan: `key='tag', value=X`.
- Whether `--effective` dir listing should also show *which* ancestor a
  flowed pair came from (planned: yes).
