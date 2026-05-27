#ifndef VIEWFS_VIEWFS_H
#define VIEWFS_VIEWFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIEWFS_VERSION_MAJOR 0
#define VIEWFS_VERSION_MINOR 1
#define VIEWFS_VERSION_PATCH 0

/* Schema version this build expects in the database. */
#define VIEWFS_SCHEMA_VERSION 1

/* Length of a hex-encoded object id, without trailing NUL. */
#define VFS_OID_HEX_LEN 32

/* Limits used by stack buffers in libviewfs and the CLI. */
#define VFS_PATH_MAX  4096
#define VFS_NAME_MAX  255

typedef struct {
    char hex[VFS_OID_HEX_LEN + 1];
} vfs_object_id;

typedef struct vfs_store vfs_store;

typedef enum {
    VFS_OK                = 0,
    VFS_ERR_BADARGS       = -1,
    VFS_ERR_NOMEM         = -2,
    VFS_ERR_IO            = -3,
    VFS_ERR_DB            = -4,
    VFS_ERR_NOTFOUND      = -5,
    VFS_ERR_EXISTS        = -6,
    VFS_ERR_PATH_RELATIVE = -7,
    VFS_ERR_PATH_ESCAPE   = -8,
    VFS_ERR_PATH_BADCHAR  = -9,
    VFS_ERR_NOTDIR        = -10,
    VFS_ERR_ISDIR         = -11,
    VFS_ERR_NOTEMPTY      = -12,
    VFS_ERR_ACCESS        = -13,
    VFS_ERR_AMBIGUOUS     = -14,
    VFS_ERR_CONFIG        = -15,
    VFS_ERR_VERSION       = -16,
    VFS_ERR_INTERNAL      = -99
} vfs_error;

const char *viewfs_version_string(void);
const char *vfs_error_str(vfs_error e);

/* Nanoseconds since the epoch (CLOCK_REALTIME). */
int64_t vfs_now_ns(void);

/* Emit a PostgreSQL NOTIFY on channel 'viewfs_change' announcing that
 * something changed in `view_name`. The payload is "<view>\t<parent_path>",
 * where <parent_path> identifies the directory whose child listing may
 * have changed (use "" for the view root). Errors are non-fatal: a NOTIFY
 * failure does not roll back the preceding mutation. */
void vfs_notify_path(vfs_store *s, const char *view, const char *parent_path);

/* ---------------------------------------------------------------- *
 * Path canonicalizer
 * ---------------------------------------------------------------- */

typedef struct {
    char path[VFS_PATH_MAX];     /* canonical, leading '/', no trailing '/' (except root "/") */
    char parent[VFS_PATH_MAX];   /* canonical parent dir; "" if direct child of root; unused for root */
    char name[VFS_NAME_MAX + 1]; /* final component; "" for root */
    int  is_root;
} vfs_canon_path;

vfs_error vfs_path_canonicalize(const char *input, vfs_canon_path *out);

/* ---------------------------------------------------------------- *
 * Object IDs
 * ---------------------------------------------------------------- */

vfs_error vfs_object_id_generate(vfs_object_id *out);
int       vfs_object_id_valid(const char *s);

/* ---------------------------------------------------------------- *
 * Store lifecycle
 * ---------------------------------------------------------------- */

/* Create a new backing store rooted at store_path.
 *
 *   store_path  - directory path that will hold config + content blobs;
 *                 must not already contain a config.toml unless reinit=1.
 *   pg_conninfo - libpq connection string. May be NULL/"" to rely on
 *                 PG* environment variables.
 *   pg_schema   - Postgres schema to use; NULL -> "viewfs".
 *   reinit      - allow overwriting an existing config.toml.
 *
 * On success, the schema is created (IF NOT EXISTS), migrations are
 * applied, and config.toml plus the directory skeleton are written. */
vfs_error vfs_store_create(const char *store_path,
                           const char *pg_conninfo,
                           const char *pg_schema,
                           int         reinit);

/* Open an existing store. The caller must vfs_store_close() it. */
vfs_error vfs_store_open(const char *store_path, vfs_store **out);

void vfs_store_close(vfs_store *s);

/* The last libpq or filesystem error message produced by an operation on
 * this store. Valid until the next failing operation. Never NULL. */
const char *vfs_store_last_error(vfs_store *s);

/* Accessors used by the CLI for status display. */
const char *vfs_store_path(const vfs_store *s);
const char *vfs_store_schema(const vfs_store *s);
const char *vfs_store_conninfo(const vfs_store *s);

/* The schema version recorded in the DB (max(schema_migrations.version)).
 * Returns VFS_OK and sets *out, or an error code. */
vfs_error vfs_schema_version(vfs_store *s, int *out);

/* Count rows in the named libviewfs table. table_id must be one of
 * "objects", "views", "mappings", "attributes", "tags". */
vfs_error vfs_count_rows(vfs_store *s, const char *table_id, int64_t *out);

/* Consistency-check helpers used by `viewfs check`.
 *
 * Each returns the count of rows that violate an invariant. In a healthy
 * store all of these are 0; non-zero is a bug somewhere. */
vfs_error vfs_check_mappings_bad_objref (vfs_store *s, int64_t *out);
vfs_error vfs_check_mappings_dir_invariant(vfs_store *s, int64_t *out);

/* Iterate every object in the DB and call the callback. Used by
 * `viewfs check` to cross-reference content files. (vfs_object_list
 * already does this, but is repeated here for clarity.) */

/* ---------------------------------------------------------------- *
 * Content blob store (host-filesystem side, no DB rows)
 * ---------------------------------------------------------------- */

/* Build the absolute path to an object's content file. The buffer must be
 * at least VFS_PATH_MAX bytes. */
vfs_error vfs_content_path(const vfs_store *s, const vfs_object_id *id,
                           char *buf, size_t bufsz);

/* Copy a host file into the store as the content of `id` via tmp+rename.
 * Returns the on-disk size, mode, and mtime via out_* (any may be NULL). */
vfs_error vfs_content_import_host(vfs_store *s, const char *host_path,
                                  const vfs_object_id *id,
                                  int64_t *out_size,
                                  int     *out_mode,
                                  int64_t *out_mtime_ns);

/* Create an empty content file for a freshly-created object. */
vfs_error vfs_content_create_empty(vfs_store *s, const vfs_object_id *id);

/* Remove a content file. Idempotent. */
vfs_error vfs_content_unlink(vfs_store *s, const vfs_object_id *id);

/* ---------------------------------------------------------------- *
 * Views
 * ---------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *description; /* may be NULL */
    int64_t     ctime_ns;
    int64_t     mtime_ns;
} vfs_view_row;

typedef void (*vfs_view_cb)(const vfs_view_row *row, void *ud);

vfs_error vfs_view_create(vfs_store *s, const char *name, const char *description);
vfs_error vfs_view_delete(vfs_store *s, const char *name);
vfs_error vfs_view_list  (vfs_store *s, vfs_view_cb cb, void *ud);
vfs_error vfs_view_exists(vfs_store *s, const char *name, int *out_exists);

/* ---------------------------------------------------------------- *
 * Objects
 * ---------------------------------------------------------------- */

typedef struct {
    vfs_object_id id;
    const char   *kind;        /* "file" | "symlink" */
    int64_t       size;
    int           mode;
    int64_t       ctime_ns;
    int64_t       mtime_ns;
    int64_t       atime_ns;
    const char   *source_path; /* may be NULL */
    const char   *checksum;    /* may be NULL */
} vfs_object_info;

typedef void (*vfs_object_cb)(const vfs_object_info *info, void *ud);

/* Insert an objects row of kind='file'. Does not touch content storage. */
vfs_error vfs_object_create_file(vfs_store     *s,
                                 int            mode,
                                 int64_t        size,
                                 const char    *source_path,
                                 int64_t        mtime_ns,
                                 vfs_object_id *out_id);

/* Insert an objects row using a caller-supplied id and full timestamp set.
 * Used by `object import` so the metadata row matches the just-written
 * content file. */
vfs_error vfs_object_insert_existing(vfs_store *s, const vfs_object_id *id,
                                     const char *kind, int mode, int64_t size,
                                     int64_t ctime_ns, int64_t mtime_ns,
                                     int64_t atime_ns,
                                     const char *source_path);

vfs_error vfs_object_get(vfs_store *s, const vfs_object_id *id,
                         vfs_object_info *out);

/* Resolve a unique prefix to a full object id. Returns NOTFOUND if no
 * match, AMBIGUOUS if more than one. A full id always resolves. */
vfs_error vfs_object_resolve(vfs_store *s, const char *prefix, vfs_object_id *out);

vfs_error vfs_object_list      (vfs_store *s, vfs_object_cb cb, void *ud);
vfs_error vfs_object_list_orphans(vfs_store *s, vfs_object_cb cb, void *ud);

/* Remove the objects row AND content. Caller decides whether to also delete
 * mappings — by default mappings are orphan-detached via FK ON DELETE SET NULL,
 * so this should be called only after the caller is sure that's intended. */
vfs_error vfs_object_delete(vfs_store *s, const vfs_object_id *id);

/* ---------------------------------------------------------------- *
 * Mappings
 * ---------------------------------------------------------------- */

typedef struct {
    const char   *view_name;
    const char   *view_path;
    const char   *parent_path;
    const char   *name;
    const char   *entry_kind;  /* "file" | "dir" | "symlink" */
    vfs_object_id object_id;   /* zeroed if entry_kind="dir" */
    int           has_object;  /* 0 for dir rows */
    int64_t       ctime_ns;
    int64_t       mtime_ns;
} vfs_mapping_row;

typedef void (*vfs_mapping_cb)(const vfs_mapping_row *row, void *ud);

/* Add a file mapping. Auto-creates missing parent directories.
 * Errors with VFS_ERR_EXISTS if the exact view_path already exists. */
vfs_error vfs_mapping_add_file(vfs_store *s, const char *view, const char *view_path,
                               const vfs_object_id *id);

/* Explicit directory mkdir. Auto-creates missing parents. */
vfs_error vfs_mapping_add_dir (vfs_store *s, const char *view, const char *view_path,
                               int mode);

/* Remove one mapping. For directories this errors with VFS_ERR_NOTEMPTY if
 * the dir still has children. */
vfs_error vfs_mapping_remove  (vfs_store *s, const char *view, const char *view_path);

vfs_error vfs_mapping_list_view  (vfs_store *s, const char *view,
                                  vfs_mapping_cb cb, void *ud);
vfs_error vfs_mapping_list_object(vfs_store *s, const vfs_object_id *id,
                                  vfs_mapping_cb cb, void *ud);

/* ---------------------------------------------------------------- *
 * Attributes
 * ---------------------------------------------------------------- */

typedef struct {
    const char *key;
    const char *value;
    int64_t     ctime_ns;
    int64_t     mtime_ns;
} vfs_attr_row;

typedef void (*vfs_attr_cb)(const vfs_attr_row *row, void *ud);

vfs_error vfs_attr_set   (vfs_store *s, const vfs_object_id *id,
                          const char *key, const char *value);
vfs_error vfs_attr_get   (vfs_store *s, const vfs_object_id *id,
                          vfs_attr_cb cb, void *ud);
vfs_error vfs_attr_remove(vfs_store *s, const vfs_object_id *id, const char *key);

/* ---------------------------------------------------------------- *
 * Tags
 * ---------------------------------------------------------------- */

typedef void (*vfs_tag_cb)(const char *tag, int64_t ctime_ns, void *ud);

vfs_error vfs_tag_add   (vfs_store *s, const vfs_object_id *id, const char *tag);
vfs_error vfs_tag_remove(vfs_store *s, const vfs_object_id *id, const char *tag);
vfs_error vfs_tag_list  (vfs_store *s, const vfs_object_id *id,
                         vfs_tag_cb cb, void *ud);

/* ---------------------------------------------------------------- *
 * Find
 * ---------------------------------------------------------------- */

vfs_error vfs_find_by_tag (vfs_store *s, const char *tag,
                           vfs_object_cb cb, void *ud);
vfs_error vfs_find_by_attr(vfs_store *s, const char *key, const char *value,
                           vfs_object_cb cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif
