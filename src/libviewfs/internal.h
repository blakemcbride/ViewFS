#ifndef VIEWFS_INTERNAL_H
#define VIEWFS_INTERNAL_H

#include <libpq-fe.h>
#include <stdint.h>

#include "viewfs/viewfs.h"

#define VFS_DEFAULT_SCHEMA "viewfs"
#define VFS_SHARD_DEPTH    1
#define VFS_LASTERR_LEN    1024
#define VFS_CONFIG_FILE    "config.toml"
#define VFS_OBJECTS_DIR    "objects"
#define VFS_TMP_DIR        "tmp"
#define VFS_DAEMONS_DIR    "daemons"
#define VFS_LOGS_DIR       "logs"

struct vfs_store {
    char    store_path[VFS_PATH_MAX];
    char    schema[64];
    char    conninfo[1024];
    int     store_version;
    int     shard_depth;
    PGconn *pg;
    char    last_error[VFS_LASTERR_LEN];
};

/* internal: set last_error from an arbitrary printf-style message */
void vfs_seterr(vfs_store *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* internal: set last_error from the most recent libpq error */
void vfs_seterr_pq(vfs_store *s, const PGresult *r);

/* internal: returns 1 if name is [A-Za-z_][A-Za-z0-9_]{0,62}. */
int vfs_ident_ok(const char *s);

/* internal: timestamp in ns since the epoch (monotonic-ish CLOCK_REALTIME). */
int64_t vfs_now_ns(void);

/* internal: build "/path/to/store/objects/aa/<id>" into buf. */
vfs_error vfs_content_path_internal(const vfs_store *s, const vfs_object_id *id,
                                    char *buf, size_t bufsz);

/* internal: ensure store skeleton dirs exist on disk. */
vfs_error vfs_store_mkskel(const char *store_path);

/* internal: load config.toml into a freshly-allocated vfs_store. */
vfs_error vfs_store_load_config(vfs_store *s);

/* internal: serialize the current vfs_store config back to config.toml. */
vfs_error vfs_store_save_config(const vfs_store *s);

/* internal: apply pending migrations to the open PGconn. */
vfs_error vfs_apply_migrations(vfs_store *s);

/* internal: run a query expected to return zero rows; set last_error on
 * failure. Returns VFS_OK on PGRES_COMMAND_OK or PGRES_TUPLES_OK. */
vfs_error vfs_exec_simple(vfs_store *s, const char *sql);

#endif
