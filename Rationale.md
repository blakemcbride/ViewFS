# Why ViewFS — The Case for a Property-Driven View Filesystem

**Thesis:** a file's location should be a *consequence* of what the file is,
not a decision you make once and pay for forever. ViewFS replaces "put the
file somewhere" with "describe the file, and let every directory that cares
collect it." That single inversion removes a surprising amount of friction
from how we live with large file collections.

---

## 1. The problem: one tree, placed by hand

Every mainstream filesystem forces each file into exactly one location in one
hierarchy. That model is simple, durable, and — past a few thousand files —
quietly expensive:

- **A file is rarely about one thing.** A photo is *2019*, *Italy*,
  *family*, and *raw*. A contract is *client-acme*, *signed*, *2024*, and
  *legal*. The single-tree model makes you pick one axis to be "the" folder
  and demotes the rest to your memory.
- **Placement is a tax you pay up front and again at retrieval.** You decide
  where something "goes," then later you have to reconstruct that decision to
  find it. Different tasks want different organizations of the *same* files,
  and the tree can only express one.
- **The usual escape hatches leak.** Symlinks and hard links bolt a second
  view onto a first, but they are manual, fragile, and asymmetric — you still
  maintain a primary home plus a thicket of links that rot when things move.
  Tag add-ons (Finder tags, `xattr`, desktop search indexes) sit *beside* the
  filesystem; your shell, scripts, and editors don't see them as directories.
  Saved searches help you *find* but not *organize* — you can't `cd` into a
  query, create a file in it, and have that mean something.

## 2. The idea: directories are filters, membership is computed

In ViewFS the metadata leads and the hierarchy follows:

- Each **object** (file) carries a set of multi-valued **properties** —
  `author=blake`, `year=2024`, `kind=report`, `tag=draft`.
- Each **directory** carries a set of property/value pairs — a **filter**.
- A file appears in a directory **exactly when** its properties are a
  superset of that directory's filter. Nothing records "this file lives
  here." A directory's contents are *computed*, every time, from properties.

The consequences fall out for free:

- **One file, many homes, zero copies.** The same object surfaces in
  `/by-author/blake`, `/reports/2024`, and `/tag/urgent` simultaneously,
  because it matches all three filters. Edit it through any of them; it's the
  same bytes.
- **Views are whole alternate hierarchies.** A `work` view and an `archive`
  view can organize the very same objects along completely different axes —
  each view is just a different tree of filters over the shared property
  space.
- **Organizing is describing.** To file something, you give it properties (or
  drop it into a directory, which *assigns* that directory's properties for
  you). To re-file it, you change its properties — there is no "move the
  bytes" step.

ViewFS also makes the *shape* of organization tunable. A directory's filter
pair can **flow** to its children (cumulative, so a child is a strict subset
of its parent — classic drill-down) or stay **independent** (the child filters
on its own axis regardless of the parent). The same mechanism expresses both
"`/2024/blake`" drill-downs and orthogonal facet trees, per pair, by choice.

## 3. Why this is more than tags-with-extra-steps

Tagging systems have existed for years; ViewFS's bet is that the *filesystem
interface itself* is what makes the model usable:

- **It is a real filesystem, through FUSE.** `ls`, `cat`, `cp`, `mv`, `grep
  -r`, your editor, your build script, `rsync` — everything that speaks POSIX
  works, unmodified. A "view" isn't a special app; it's a mount point. That
  is the difference between a feature and a place you live.
- **Creating and moving carry semantics.** `cp file.txt into /reports/2024/`
  doesn't just copy bytes — the new object inherits that directory's filter,
  so it *belongs* there immediately. `mv` between directories swaps the source
  directory's pairs for the destination's and keeps the rest: moving is
  re-describing, expressed in the gesture you already use.
- **Query and navigation are the same act.** Browsing `/kind=report/year=2024`
  *is* running the query `kind=report AND year=2024`. There's no separate
  search box whose results you then can't operate on as files.
- **Metadata is first-class and durable.** Properties live in a real
  relational store (PostgreSQL), are multi-valued, are indexed for the
  superset-match query, and are inspectable and queryable from a CLI
  (`find --prop`) as well as through the mount (`getfattr`/`setfattr`).

## 4. Why not just symbolic or hard links?

Links are the closest thing a conventional filesystem offers to "one file in
several places," so they deserve a direct answer. The short version: **a link
is a static pointer you author and maintain by hand; a ViewFS appearance is a
computed consequence of what the file is.** That difference is the whole game.

**Symbolic links** are manual aliases layered on top of a primary home:

- **You create and maintain every one by hand.** To make a file show up in
  five organizational places you author five symlinks — and re-author them
  whenever anything changes. ViewFS membership is automatic: set a property
  (or it already matches) and the file appears in every directory whose
  filter it satisfies, with nothing to create.
- **They presuppose a "real" location plus aliases.** A symlink points *from*
  an alias *to* a canonical original; the tree stays primary and the links are
  second-class. ViewFS has no primary home — identity lives with the object,
  and every appearance is equal and derived.
- **They rot.** Move or rename the target and the link dangles; the alias
  graph drifts out of sync with reality and you find out only when something
  breaks. There is nothing to dangle in ViewFS: appearances are recomputed
  from properties, so renaming or re-describing an object updates every place
  it shows up at once.
- **They encode location, not meaning.** A symlink records "this path → that
  path." It cannot tell you *why* the file is there, can't be queried, and two
  unrelated links look identical. A ViewFS appearance exists *because*
  `author=blake` (or whatever pair) — the reason is explicit, inspectable, and
  the very basis of `find`.
- **They're a security footgun.** A symlink target can point anywhere,
  including outside the collection; absolute targets escape. A computed
  appearance can only ever surface objects from the pool.

**Hard links** come closer — one inode, several names, genuinely the same
bytes — but they're still manual and meaningless:

- **Still authored by hand, one name at a time.** Nothing is computed;
  reorganizing a collection along a new axis is still a scripting exercise in
  creating names.
- **Bound to one filesystem, and (normally) files only.** Hard links can't
  cross devices and can't link directories, so you can't compose whole
  subtrees. ViewFS membership is logical and composes directory filters
  freely.
- **No reason, no query.** A hard link is just another name on an inode; it
  carries no metadata, no intent, and no answer to "show me everything that is
  a 2024 report." In ViewFS, the appearances *are* that query.
- **The bookkeeping is on you.** Link-count and deletion semantics — the bytes
  survive until the last name is removed — are subtle to reason about. ViewFS
  has one clear rule: changing where a file appears means changing its
  properties, and deleting the object is an explicit, separate act.

The common thread: links make you **maintain a second structure by hand**
that has to be kept consistent with the first. ViewFS keeps **one source of
truth** — the object's properties — and derives every appearance from it, so
there is no link graph to build, audit, or repair. Links bolt extra names
onto the one true tree; ViewFS retires the idea that there *is* one true tree.

## 5. Where it pays off

- **Research / document libraries:** the same paper sits under author, year,
  project, and reading-status, and you stop choosing which folder "wins."
- **Media and asset collections:** photos and footage organized by date,
  location, subject, and rating at once — add a rating and it appears in the
  rating view with no re-shuffling.
- **Engineering and ops artifacts:** logs, reports, and configs filed by
  service, environment, severity, and date; build a per-incident view by
  composing filters rather than copying files around.
- **Shared, multi-perspective workspaces:** a team exposes the same object
  pool as different views for different roles — each role's tree is its own
  set of filters, no duplication, one source of truth for the bytes.

## 6. What the prototype proves (and what it doesn't)

This repository is a working proof of the model, not a product. It
demonstrates the hard parts end-to-end: computed superset membership with
multi-valued properties, per-pair flow, the same object surfacing across many
directories and views, POSIX read/write/create/move/delete through a FUSE
mount with create-assigns-properties and move-swaps-properties semantics,
property-driven `find`, and a self-checking store with a clean teardown path.

It is deliberately **not** a production filesystem: it layers over an existing
host filesystem via FUSE rather than living in the kernel, keeps metadata in a
PostgreSQL schema, serializes daemon metadata operations for simplicity, and
makes no claim to quotas, encryption, multi-host consistency, or
crash-consistency beyond "writes completed by `close(2)` are durable." Those
are real limits — and they're the *right* limits for proving an idea cheaply.

## 7. The bet

The single-rooted, place-it-by-hand directory tree is one of computing's most
unexamined defaults. ViewFS argues that the tree should be a *view* —
derived, plural, and cheap to reshape — while identity and meaning live with
the object. If that inversion holds up, the payoff is concrete: you describe a
file once and find it from every angle that matters, using the tools you
already have. This prototype exists to show that the inversion is not just
appealing in the abstract but workable in practice, at the level of `ls` and
`cat`.
