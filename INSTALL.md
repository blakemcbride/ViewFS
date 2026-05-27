# Installing ViewFS

This document explains how to build ViewFS from source, set up the
PostgreSQL backend it needs, optionally install the binaries
system-wide, and verify the result.

For a tutorial on **using** the system after install, see `RUNNING.md`.

---

## 1. Prerequisites

ViewFS is a Linux-only research prototype. The development target is
Fedora 44+; any modern Linux with the right packages should work, but
the package names below are Fedora-flavored.

You need:

- **Linux kernel** with FUSE 3 support enabled (any modern distro).
- **libfuse 3** runtime + headers.
- **libpq** (PostgreSQL client library) runtime + headers.
- **A C11 compiler** (GCC or Clang) and **GNU make**.
- **`pkg-config`** for resolving compile/link flags.
- **A running PostgreSQL server** (local or remote) the daemon and CLI
  can connect to.
- **`psql`** (the PostgreSQL command-line client), used by the demo
  script and integration tests.
- **`fusermount3`** — provided by the `fuse3` package — for unmounting.

### Fedora 44+

```sh
sudo dnf install fuse3 fuse3-devel libpq libpq-devel \
                 gcc make pkgconf-pkg-config \
                 postgresql postgresql-server
```

You can swap `postgresql-server` for any other source of a reachable
Postgres instance — a container, a remote server, etc.

### Debian / Ubuntu (untested but should work)

```sh
sudo apt install fuse3 libfuse3-dev libpq-dev \
                 gcc make pkg-config \
                 postgresql postgresql-client
```

### Arch (untested)

```sh
sudo pacman -S fuse3 postgresql-libs gcc make pkgconf postgresql
```

Confirm the dev libs are visible to `pkg-config`:

```sh
pkg-config --modversion fuse3 libpq
# should print two version numbers, e.g.
#   3.18.2
#   18.0
```

If either line is missing, the corresponding `-devel`/`-dev` package
isn't installed.

---

## 2. Get the source

```sh
git clone <repository-url> viewfs
cd viewfs
```

(If you already have the source as a directory, just `cd` into it.)

---

## 3. Set up PostgreSQL

ViewFS keeps every piece of metadata (views, mappings, objects, tags,
attributes) in PostgreSQL. Content blobs are kept as ordinary files on
the host filesystem.

### Start the server (Fedora)

If you installed `postgresql-server` and this is a brand-new instance:

```sh
sudo postgresql-setup --initdb     # first time only
sudo systemctl enable --now postgresql
```

### Create a database

You can connect as the system `postgres` superuser, or as a dedicated
user — both work. The simplest "dev box" setup uses the `postgres`
role over the local Unix socket:

```sh
sudo -u postgres createdb viewfs
```

To connect as your own login user instead (so you don't need `sudo -u`
for every operation):

```sh
sudo -u postgres createuser -s "$USER"
sudo -u postgres createdb -O "$USER" viewfs
```

### Verify the connection works

```sh
psql 'host=/var/run/postgresql user=postgres dbname=viewfs' \
     -c 'select current_user, current_database();'
```

If you get a "FATAL: Peer authentication failed" error, your
`pg_hba.conf` may need a `trust` or `md5` line for the local socket.
For a dev box, the file at `/var/lib/pgsql/data/pg_hba.conf` (Fedora
path) typically already has:

```
local   all   all   trust
host    all   all   127.0.0.1/32   trust
```

which is fine for prototyping. Restart the server if you edit it:

```sh
sudo systemctl restart postgresql
```

---

## 4. Build

From the project root:

```sh
make
```

This produces two binaries in the project directory:

- `./viewfs`       — the admin CLI
- `./viewfs-fuse`  — the FUSE daemon

A clean rebuild:

```sh
make clean && make
```

A typical first build takes a few seconds. The compile flags are
`-std=c11 -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration -O2 -g`;
the build is clean (zero warnings) on a supported Fedora system.

---

## 5. Verify the build (optional but recommended)

ViewFS ships a test suite that covers spec §17's twenty numbered tests
plus a crash-recovery test. Running it confirms that your PostgreSQL
setup, the FUSE installation, and the binaries themselves all work
together.

```sh
make test
```

You should see output ending with something like:

```
==> Integration suite
test_attrs                       ... ok
test_crash                       ... ok
test_cross_view                  ... ok
...
integration: passed 14, failed 0
```

If the harness can't reach PostgreSQL it will say so up front; override
the connection string with `VIEWFS_TEST_PG`:

```sh
VIEWFS_TEST_PG='host=localhost user=postgres dbname=viewfs' make test
```

If the suite passes, your install is ready.

---

## 6. Try the end-to-end demonstration

`examples/demo.sh` walks through spec §18 — initializing a fresh
store, creating two views, importing files, mapping the same file into
both views under different paths, mounting both, writing through one
view, observing the change through the other, renaming, unmounting,
remounting, persistence.

```sh
./examples/demo.sh              # interactive, pauses between each step
./examples/demo.sh --unattended # CI-style, no pauses
```

The demo wipes its own dedicated schema (`viewfs_demo`) and scratch
directory on each invocation so it never touches a real store.

---

## 7. Install system-wide (optional)

For development you can keep using `./viewfs` and `./viewfs-fuse`
from the build directory — that's how the integration tests run.

For a real install:

```sh
sudo make install                  # installs to /usr/local/bin
# or
sudo make install PREFIX=/opt/viewfs
# or for packagers (staging dir):
make install DESTDIR=/tmp/staging PREFIX=/usr
```

Both binaries land in `$PREFIX/bin` and the CLI's daemon discovery
picks them up automatically — `viewfs mount` looks for `viewfs-fuse`
next to itself via `/proc/self/exe`, then falls back to `PATH`.

To remove:

```sh
sudo make uninstall
```

---

## 8. Configure your shell

The CLI needs to know which backing store to operate on. You can pass
`--store /path/to/store` to every command, or set the environment
variable once:

```sh
export VIEWFS_STORE="$HOME/.local/share/viewfs"
```

Initialize that store (creates the directory and the PostgreSQL schema).
You can either pass `--pg CONNINFO` explicitly, or set
`VIEWFS_PG_USER` and `VIEWFS_PG_DATABASE` and let `init` pick them up:

```sh
export VIEWFS_PG_USER=postgres
export VIEWFS_PG_DATABASE=viewfs
viewfs init "$VIEWFS_STORE"
```

For anything beyond user and database (custom host, port, sslmode,
service file, etc.) pass `--pg CONNINFO` with a full libpq connection
string.

You're now ready for the tutorial in `RUNNING.md`.

---

## 9. Troubleshooting

**`pkg-config: fuse3 not found` during build.**
You're missing `fuse3-devel` (Fedora) or `libfuse3-dev` (Debian).

**`error: postgres: FATAL: role "X" does not exist`.**
Either create the role (`sudo -u postgres createuser -s "$USER"`) or
include `user=postgres` in your conninfo and rely on local-socket
trust auth.

**`viewfs unmount` says "fusermount3: not found".**
Install the `fuse3` package (which provides the setuid helper).

**`make test` hangs.**
Probably a stale FUSE mount from an aborted previous run. Clean up:

```sh
mount | awk '/viewfs/ {print $3}' | xargs -r -n1 fusermount3 -uz
```

**Integration tests fail with "cannot reach Postgres."**
Confirm `psql "$VIEWFS_TEST_PG" -c 'select 1'` works first; the
harness uses the same string.

**Daemon won't start with "schema 'viewfs' does not exist".**
You probably edited `config.toml` to point at a schema name that was
never created. Re-run `viewfs init --reinit` against the same store
path with the right `--schema` flag, or drop the bad schema and let
init recreate it.

**Where does data live?**
- PostgreSQL schema (configurable, default `viewfs`): every row.
- `$STORE/objects/<aa>/<id>`: raw file contents, one file per object.
- `$STORE/config.toml`: store-local config; reading this tells you
  the conninfo and schema name.

---

## 10. Uninstall

```sh
sudo make uninstall                                # if you ran `make install`
rm -rf "$VIEWFS_STORE"                             # remove your store directory
psql 'host=/var/run/postgresql user=postgres dbname=viewfs' \
     -c 'DROP SCHEMA viewfs CASCADE'               # drop the PG schema
```

That's it — ViewFS leaves nothing else on your system.
