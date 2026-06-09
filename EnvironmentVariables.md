# Environment Variables

Every environment variable that affects ViewFS, grouped by what reads it.
"Required" means the named tool fails (or can't do useful work) without it.

## 1. Read by the `viewfs` CLI and `viewfs-fuse` daemon

| Variable | Required? | Default | Read by / notes |
|---|---|---|---|
| `VIEWFS_STORE` | Effectively yes, unless you pass the path another way | *(none)* | Backing-store directory. Used by every CLI command (as the fallback for `--store`) and by `viewfs mount`. You can instead pass `--store PATH` on any command, or the positional `STORE_PATH` to `viewfs init`. `--store` also *sets* `VIEWFS_STORE` for the rest of that process. |
| `VIEWFS_PG_USER` | No | libpq default (`PGUSER`, else your OS login name) | Consulted **only by `viewfs init`**, and **only when `--pg` is not given**, to synthesize the PostgreSQL conninfo. |
| `VIEWFS_PG_DATABASE` | No | `viewfs` | Consulted **only by `viewfs init`** when `--pg` is not given. |

`viewfs init` **creates the database if it does not exist** (it connects to a
maintenance database — `postgres`, falling back to `template1` — with the
same host/user and issues `CREATE DATABASE`), provided the connecting role
has the `CREATEDB` privilege. It then creates the schema and applies
migrations.

After `viewfs init`, the full PostgreSQL conninfo and schema name are written
into `$STORE/config.toml`. **Every later command and the daemon read the
conninfo from there**, so `VIEWFS_PG_USER` / `VIEWFS_PG_DATABASE` are not
needed again.

### Minimum to get going
```sh
export VIEWFS_STORE=/path/to/store     # the one var worth setting
./viewfs init                          # uses $VIEWFS_STORE; PG via libpq defaults
```
If your local PostgreSQL needs a specific role (e.g. the dev/test setup uses
`postgres`), either also `export VIEWFS_PG_USER=postgres` before `init`, or
pass `viewfs init --pg "host=/var/run/postgresql user=postgres dbname=viewfs"`.

## 2. Standard libpq variables (fallback at connect time)

ViewFS does not read these itself; **libpq** does, for any connection field
not supplied via `--pg` or `config.toml`. All optional; all use libpq's own
defaults. The common ones:

| Variable | Default | Notes |
|---|---|---|
| `PGHOST` | local Unix socket (e.g. `/var/run/postgresql`) | Server host or socket dir. |
| `PGPORT` | `5432` | Server port. |
| `PGUSER` | OS login name | Role to connect as. |
| `PGPASSWORD` | *(none)* | Password, if the auth method requires one. |
| `PGDATABASE` | value of `PGUSER` | Database name. |

(Other `PG*` variables documented by libpq, such as `PGOPTIONS` or
`PGSSLMODE`, are honored too.)

## 3. Read by `DestroyAll.sh`

`DestroyAll.sh` is **teardown-only**: it unmounts any live ViewFS mounts, drops the
whole Postgres **database**, and deletes the object store. It does **not**
recreate anything — run `viewfs init` afterward (which recreates the database
and schema).

| Variable | Required? | Default | Notes |
|---|---|---|---|
| `VIEWFS_STORE` | **Yes** | *(none — the script errors if unset)* | The object store directory to delete. |
| `VIEWFS_DB` | No | `viewfs` | Database to drop (`DROP DATABASE … WITH (FORCE)`). Built-in databases (`postgres`/`template0`/`template1`) are refused. |
| `VIEWFS_PG_MAINT` | No | `host=/var/run/postgresql user=postgres dbname=postgres` | Maintenance conninfo used to issue the drop (you cannot drop the database you are connected to). |

## 4. Read by `examples/demo.sh`

| Variable | Required? | Default | Notes |
|---|---|---|---|
| `VIEWFS_DEMO_STORE` | No | `/tmp/viewfs-demo` | Demo backing-store directory. |
| `VIEWFS_DEMO_MNT` | No | `/tmp/viewfs-demo-mnt` | Demo mountpoint root. |
| `VIEWFS_DEMO_PG` | No | `host=/var/run/postgresql user=postgres dbname=viewfs` | libpq conninfo for the demo. |
| `VIEWFS_DEMO_SCHEMA` | No | `viewfs_demo` | Demo schema (dropped + recreated each run). |

## 5. Read by the integration tests (`tests/integration/`)

| Variable | Required? | Default | Notes |
|---|---|---|---|
| `VIEWFS_TEST_PG` | No | `host=/var/run/postgresql user=postgres dbname=viewfs` | libpq conninfo for the test harness (`lib.sh`). Each test creates its own ephemeral schema and store. |

(The DB-backed unit test `tests/unit/test_props` honors the same
`VIEWFS_TEST_PG`, falling back to the same default, and skips cleanly if the
server is unreachable.)

## 6. Build-time variables (`make`)

Overridable `make`/environment variables in the `Makefile`. All optional.

| Variable | Default | Notes |
|---|---|---|
| `CC` | `gcc` | C compiler. |
| `CFLAGS` | `-std=c11 -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration -O2 -g` | Compiler flags. |
| `LDFLAGS` | *(empty)* | Extra linker flags. |
| `PREFIX` | `/usr/local` | Install prefix for `make install` (binaries go to `$PREFIX/bin`). |
| `DESTDIR` | *(empty)* | Staging directory for packagers. |
