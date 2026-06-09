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
 * store all of these are 0; non-zero is a bug somewhere.
 *   _dirs_reachable  -- directory rows whose parent_path names a missing dir
 *   _props_orphans   -- object_props rows referencing a missing object */
vfs_error vfs_check_dirs_reachable(vfs_store *s, int64_t *out);
vfs_error vfs_check_props_orphans (vfs_store *s, int64_t *out);
vfs_error vfs_check_dir_structure (vfs_store *s, int64_t *out);

/* ---------------------------------------------------------------- *
 * Content blob store (host-filesystem side, no DB rows)
 * ---------------------------------------------------------------- */

/* Build the absolute path to an object's content file. The buffer must be
 * at least VFS_PATH_MAX bytes. */
vfs_error vfs_content_path(const vfs_store *s, const vfs_object_id *id,
                           char *buf, size_t bufsz);

/* Copy a host file into the store as the content of `id` via tmp+rename.
 * Returns the on-disk size, mode, and mtime via out_* (any may be NULL).
 * If checksum_hex_out is non-NULL, writes the SHA-256 of the copied
 * bytes (64 lowercase hex + NUL) into that 65-byte buffer.
 * If checksum_state_out is non-NULL, writes the SHA-256 intermediate
 * state (suitable for resuming an append) — buffer must be at least
 * 128 bytes; the actual length is written to *checksum_state_len_out. */
vfs_error vfs_content_import_host(vfs_store *s, const char *host_path,
                                  const vfs_object_id *id,
                                  int64_t *out_size,
                                  int     *out_mode,
                                  int64_t *out_mtime_ns,
                                  char    *checksum_hex_out,
                                  void    *checksum_state_out,
                                  size_t  *checksum_state_len_out);

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
    const char   *kind;             /* "file" | "symlink" */
    const char   *name;             /* intrinsic display name; "" if unset */
    int64_t       size;
    int           mode;
    int           uid;              /* -1 if column was NULL */
    int           gid;              /* -1 if column was NULL */
    int64_t       ctime_ns;
    int64_t       mtime_ns;
    int64_t       atime_ns;
    const char   *source_path;      /* may be NULL */
    const char   *checksum;         /* may be NULL (always NULL for symlinks) */
    int           has_checksum_state; /* 1 if checksum_state column is non-NULL */
} vfs_object_info;

typedef void (*vfs_object_cb)(const vfs_object_info *info, void *ud);

/* Insert an objects row of kind='file'. Does not touch content storage.
 * `name` is the intrinsic display name (may be NULL/"" to leave empty).
 * uid/gid/checksum/state may be -1 / -1 / NULL / NULL+0 to leave NULL.
 * If state is non-NULL, state_len must be > 0. */
vfs_error vfs_object_create_file(vfs_store     *s,
                                 const char    *name,
                                 int            mode,
                                 int64_t        size,
                                 int            uid,
                                 int            gid,
                                 const char    *checksum,
                                 const void    *state,
                                 size_t         state_len,
                                 const char    *source_path,
                                 int64_t        mtime_ns,
                                 vfs_object_id *out_id);

/* Insert an objects row using a caller-supplied id and full timestamp set.
 * Used by `object import` so the metadata row matches the just-written
 * content file. `name` is the intrinsic display name (may be NULL/"").
 * uid/gid/checksum/state may be -1 / -1 / NULL / NULL+0. */
vfs_error vfs_object_insert_existing(vfs_store *s, const vfs_object_id *id,
                                     const char *kind, const char *name,
                                     int mode, int64_t size,
                                     int uid, int gid,
                                     const char *checksum,
                                     const void *state, size_t state_len,
                                     int64_t ctime_ns, int64_t mtime_ns,
                                     int64_t atime_ns,
                                     const char *source_path);

/* Set the intrinsic display name of an existing object. */
vfs_error vfs_object_set_name(vfs_store *s, const vfs_object_id *id,
                              const char *name);

/* Update uid and/or gid on an existing object. Pass -1 for either field
 * to leave it unchanged (the conventional chown sentinel). */
vfs_error vfs_object_set_owner   (vfs_store *s, const vfs_object_id *id,
                                  int uid, int gid);

/* Atomically set both the checksum (hex) and the intermediate state
 * columns on an existing object. Pass (NULL, NULL, 0) to clear both. */
vfs_error vfs_object_set_checksum(vfs_store *s, const vfs_object_id *id,
                                  const char *checksum_hex,
                                  const void *state, size_t state_len);

/* Set POSIX mode bits on an existing object. */
vfs_error vfs_object_set_mode(vfs_store *s, const vfs_object_id *id, int mode);

/* Update size + mtime + atime on an existing object (used by the FUSE
 * daemon when a writable file is closed). */
vfs_error vfs_object_set_stat(vfs_store *s, const vfs_object_id *id,
                              int64_t size, int64_t mtime_ns, int64_t atime_ns);

/* Update mtime + atime only (used by utimens; leaves size untouched). */
vfs_error vfs_object_set_times(vfs_store *s, const vfs_object_id *id,
                               int64_t mtime_ns, int64_t atime_ns);

/* Insert a kind='symlink' object whose content is the target string.
 * size is set to strlen(target). uid/gid may be -1 to leave NULL. */
vfs_error vfs_object_create_symlink(vfs_store *s, const char *name,
                                    const char *target, int uid, int gid,
                                    vfs_object_id *out_id);

/* Fetch a symlink object's target into buf. VFS_ERR_NOTFOUND if the object
 * has no symlink_target (e.g. it is a file). */
vfs_error vfs_object_symlink_target(vfs_store *s, const vfs_object_id *id,
                                    char *buf, size_t bufsz);

/* ---------------------------------------------------------------- *
 * SHA-256 helpers (used by import, FUSE write path, and check verify)
 * ---------------------------------------------------------------- */

/* Number of bytes needed to snapshot the streaming SHA-256 state.
 * Matches OpenSSL's SHA256_CTX layout. */
#define VFS_SHA256_STATE_LEN 112

/* Streaming SHA-256 context. `ctx` is opaque; treat the struct as a
 * by-value handle that's safe to keep on the stack. */
typedef struct vfs_sha256_stream {
    void *ctx;
} vfs_sha256_stream;

/* SHA-256 of the file at `path`, written as 64 lowercase hex + NUL. */
vfs_error vfs_sha256_hex_path       (const char *path, char hex_out[65]);

/* Streaming API: init → update*N → (snapshot|peek_hex)? → finalize. */
vfs_error vfs_sha256_stream_init    (vfs_sha256_stream *st);
vfs_error vfs_sha256_stream_update  (vfs_sha256_stream *st,
                                     const void *buf, size_t n);
/* Copy the in-progress state into a VFS_SHA256_STATE_LEN-byte buffer.
 * Stream remains usable; restore later with vfs_sha256_stream_restore. */
vfs_error vfs_sha256_stream_snapshot(vfs_sha256_stream *st, void *state_out);
/* Peek at the would-be final hex digest without consuming the stream. */
vfs_error vfs_sha256_stream_peek_hex(vfs_sha256_stream *st, char hex_out[65]);
/* Finalize: writes the hex digest and tears down the context. */
vfs_error vfs_sha256_stream_finalize(vfs_sha256_stream *st, char hex_out[65]);
/* Tear down without producing a digest. */
void      vfs_sha256_stream_abort   (vfs_sha256_stream *st);
/* Resume a previously-snapshotted stream. state_len must equal
 * VFS_SHA256_STATE_LEN. After restore the stream behaves as if every
 * byte hashed into the original stream had just been re-fed. */
vfs_error vfs_sha256_stream_restore (vfs_sha256_stream *st,
                                     const void *state_in, size_t state_len);

vfs_error vfs_object_get(vfs_store *s, const vfs_object_id *id,
                         vfs_object_info *out);

/* Resolve a unique prefix to a full object id. Returns NOTFOUND if no
 * match, AMBIGUOUS if more than one. A full id always resolves. */
vfs_error vfs_object_resolve(vfs_store *s, const char *prefix, vfs_object_id *out);

vfs_error vfs_object_list      (vfs_store *s, vfs_object_cb cb, void *ud);

/* List objects that carry NO properties. In the property model such an
 * object matches only empty-filter directories (view roots), so it is
 * "loose" — the nearest analogue to the old orphan concept. */
vfs_error vfs_object_list_orphans(vfs_store *s, vfs_object_cb cb, void *ud);

/* Remove the objects row (cascading object_props) AND its content blob.
 * This is the explicit, destructive delete; it removes the object from
 * every view/directory it matched. (FUSE unlink also routes here.) */
vfs_error vfs_object_delete(vfs_store *s, const vfs_object_id *id);

/* ---------------------------------------------------------------- *
 * Object properties (multi-valued; key may carry several values).
 * These drive view membership. A "tag" is sugar for key='tag'.
 * ---------------------------------------------------------------- */

typedef struct {
    const char *key;
    const char *value;
    int64_t     ctime_ns;
    int64_t     mtime_ns;
} vfs_prop_row;

typedef void (*vfs_prop_cb)(const vfs_prop_row *row, void *ud);

/* Add one (key,value). Idempotent: re-adding an existing pair is a no-op. */
vfs_error vfs_object_prop_add(vfs_store *s, const vfs_object_id *id,
                              const char *key, const char *value);

/* Remove a single (key,value), or every value of `key` when value==NULL.
 * Returns VFS_ERR_NOTFOUND if nothing matched. */
vfs_error vfs_object_prop_unset(vfs_store *s, const vfs_object_id *id,
                                const char *key, const char *value);

vfs_error vfs_object_prop_list(vfs_store *s, const vfs_object_id *id,
                               vfs_prop_cb cb, void *ud);

/* ---------------------------------------------------------------- *
 * View directories (the per-view tree) and their property filters
 * ---------------------------------------------------------------- */

typedef struct {
    const char *view_name;
    const char *dir_path;     /* canonical, leading '/'; root is "/" */
    const char *parent_path;  /* "" for root; "/" for a child of root */
    const char *name;         /* final component; "" for root */
    int         mode;
    int64_t     ctime_ns;
    int64_t     mtime_ns;
} vfs_dir_row;

typedef void (*vfs_dir_cb)(const vfs_dir_row *row, void *ud);

typedef struct {
    const char *key;
    const char *value;
    int         flow;         /* 1 if this pair flows to descendants */
    const char *source_dir;   /* dir the pair came from (for --effective) */
    int64_t     ctime_ns;
    int64_t     mtime_ns;
} vfs_dirprop_row;

typedef void (*vfs_dirprop_cb)(const vfs_dirprop_row *row, void *ud);

/* Ensure a view's root directory row ("/") exists. Idempotent. */
vfs_error vfs_dir_ensure_root(vfs_store *s, const char *view);

/* mkdir: create a directory with an empty property set, auto-creating any
 * missing ancestors. VFS_ERR_EXISTS if the exact path already exists. */
vfs_error vfs_dir_create(vfs_store *s, const char *view, const char *dir_path,
                         int mode);

/* rmdir: refuse with VFS_ERR_NOTEMPTY if the directory has child
 * directories or a non-empty membership set. Root cannot be removed. */
vfs_error vfs_dir_remove(vfs_store *s, const char *view, const char *dir_path);

/* Set *out to 1 if the path is a directory in this view, else 0. */
vfs_error vfs_dir_exists(vfs_store *s, const char *view, const char *dir_path,
                         int *out);

/* Set the mode bits on a directory row. */
vfs_error vfs_dir_set_mode(vfs_store *s, const char *view, const char *dir_path,
                           int mode);

/* Rename/move a directory subtree within a view. Carries the directory's
 * property pairs (and all descendants') with it. VFS_ERR_NOTFOUND if `from`
 * is not a directory, VFS_ERR_EXISTS if `to` already exists, VFS_ERR_BADARGS
 * for root or a move into its own descendant. */
vfs_error vfs_dir_rename(vfs_store *s, const char *view,
                         const char *from, const char *to);

/* Fetch a directory row. The returned row's strings are valid until the
 * next libviewfs call on this store. VFS_ERR_NOTFOUND if absent. */
vfs_error vfs_dir_get(vfs_store *s, const char *view, const char *dir_path,
                      vfs_dir_row *out);

/* List the immediate child directories of dir_path. */
vfs_error vfs_dir_list_children(vfs_store *s, const char *view,
                                const char *dir_path, vfs_dir_cb cb, void *ud);

/* Directory property management. `flow` marks whether the pair cascades to
 * descendant directories. _add inserts (no-op if the pair already exists);
 * _set inserts or updates the flow flag of an existing pair. _delete removes
 * one (key,value), or every value of `key` when value==NULL. */
vfs_error vfs_dir_prop_add(vfs_store *s, const char *view, const char *dir_path,
                           const char *key, const char *value, int flow);
vfs_error vfs_dir_prop_set(vfs_store *s, const char *view, const char *dir_path,
                           const char *key, const char *value, int flow);
vfs_error vfs_dir_prop_delete(vfs_store *s, const char *view, const char *dir_path,
                              const char *key, const char *value);

/* List a directory's property pairs. When effective != 0 the list includes
 * pairs inherited from ancestors via flow=true (source_dir identifies the
 * owning directory); otherwise only the directory's own pairs are listed. */
vfs_error vfs_dir_prop_list(vfs_store *s, const char *view, const char *dir_path,
                            int effective, vfs_dirprop_cb cb, void *ud);

/* ---------------------------------------------------------------- *
 * Membership (computed): the files visible in a directory
 * ---------------------------------------------------------------- */

/* Emit every object whose property set is a superset of dir_path's
 * effective property set (own pairs + flowed ancestor pairs). An empty
 * effective set matches every object. VFS_ERR_NOTFOUND if dir_path is not
 * a directory in the view. */
vfs_error vfs_dir_members(vfs_store *s, const char *view, const char *dir_path,
                          vfs_object_cb cb, void *ud);

/* Resolve a single member of dir_path by its intrinsic name.
 *   VFS_OK            -- *out set (unique match)
 *   VFS_ERR_NOTFOUND  -- no member of that name
 *   VFS_ERR_AMBIGUOUS -- more than one member shares the name */
vfs_error vfs_dir_member_by_name(vfs_store *s, const char *view,
                                 const char *dir_path, const char *name,
                                 vfs_object_id *out);

/* Emit every member of dir_path whose intrinsic name == name (there may be
 * several — the caller disambiguates, e.g. by object-id prefix). */
vfs_error vfs_dir_members_named(vfs_store *s, const char *view,
                                const char *dir_path, const char *name,
                                vfs_object_cb cb, void *ud);

/* ---------------------------------------------------------------- *
 * Find
 * ---------------------------------------------------------------- */

typedef struct {
    const char *key;
    const char *value;        /* NULL => match any value of key */
} vfs_prop_pair;

/* Emit every object whose properties satisfy ALL n pairs (AND). */
vfs_error vfs_find_by_props(vfs_store *s, const vfs_prop_pair *pairs, int n,
                            vfs_object_cb cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif
