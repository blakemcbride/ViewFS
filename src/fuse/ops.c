#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>     /* RENAME_NOREPLACE, RENAME_EXCHANGE */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ops.h"
#include "notify.h"

viewfs_ctx CTX;

/* Per-open-file state. fi->fh holds a pointer to one of these (allocated
 * in op_open / op_create, freed in op_release). The SHA-256 fields
 * implement the "incremental checksum for pure-append writes" policy:
 *
 *   sha_live=1 means the stream contains a valid running hash of the
 *   bytes currently on disk under this object id. While that's true,
 *   any op_write at offset == known_size advances the hash; any other
 *   write (out-of-order, overwrite, truncate) invalidates it, after
 *   which op_flush nulls the stored checksum + state. */
struct vfs_ofile {
    int               fd;
    int               writable;     /* opened with non-O_RDONLY */
    int               modified;     /* >=1 successful op_write or truncate */
    int               sha_live;     /* SHA state tracks current on-disk bytes */
    int64_t           known_size;   /* file size that the SHA covers */
    vfs_sha256_stream sha;
};
#define VFS_OFILE(fi)  ((struct vfs_ofile *)(uintptr_t)(fi)->fh)

static struct vfs_ofile *ofile_new(int fd, int writable) {
    struct vfs_ofile *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->fd       = fd;
    o->writable = writable;
    return o;
}

static void ofile_free(struct vfs_ofile *o) {
    if (!o) return;
    if (o->sha_live) vfs_sha256_stream_abort(&o->sha);
    if (o->fd > 0)   close(o->fd);
    free(o);
}

static void ofile_invalidate_sha(struct vfs_ofile *o) {
    if (o->sha_live) {
        vfs_sha256_stream_abort(&o->sha);
        o->sha_live = 0;
    }
}

static void trace(const char *fmt, ...) {
    if (!CTX.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "viewfs-fuse[%s]: ", CTX.view_name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Canonicalize a FUSE-supplied path into cp, returning -errno on bad input. */
static int canon(const char *in, vfs_canon_path *out) {
    vfs_error rc = vfs_path_canonicalize(in, out);
    switch (rc) {
    case VFS_OK:                return 0;
    case VFS_ERR_PATH_RELATIVE: return -EINVAL;
    case VFS_ERR_PATH_ESCAPE:   return -EACCES;
    case VFS_ERR_PATH_BADCHAR:  return -EINVAL;
    default:                    return -EIO;
    }
}

/* ------------------------------------------------------------------
 * stat fillers
 * ------------------------------------------------------------------ */

static void fill_dir_stat(struct stat *st, int mode, int64_t mtime_ns) {
    memset(st, 0, sizeof *st);
    st->st_mode  = S_IFDIR | (mode ? (mode_t)(mode & 07777) : (mode_t)0755);
    st->st_nlink = 2;
    st->st_uid   = getuid();
    st->st_gid   = getgid();
    st->st_atim.tv_sec  = mtime_ns / 1000000000;
    st->st_atim.tv_nsec = mtime_ns % 1000000000;
    st->st_mtim  = st->st_atim;
    st->st_ctim  = st->st_atim;
}

static void fill_file_stat(struct stat *st, int mode, int64_t size,
                           int uid, int gid,
                           int64_t ctime_ns, int64_t mtime_ns, int64_t atime_ns) {
    memset(st, 0, sizeof *st);
    st->st_mode  = S_IFREG | (mode_t)(mode & 07777);
    st->st_nlink = 1;
    st->st_uid   = (uid >= 0) ? (uid_t)uid : getuid();
    st->st_gid   = (gid >= 0) ? (gid_t)gid : getgid();
    st->st_size  = size;
    st->st_blocks = (size + 511) / 512;
    st->st_ctim.tv_sec  = ctime_ns / 1000000000;
    st->st_ctim.tv_nsec = ctime_ns % 1000000000;
    st->st_mtim.tv_sec  = mtime_ns / 1000000000;
    st->st_mtim.tv_nsec = mtime_ns % 1000000000;
    st->st_atim.tv_sec  = atime_ns / 1000000000;
    st->st_atim.tv_nsec = atime_ns % 1000000000;
}

/* ------------------------------------------------------------------
 * tiny DB helpers (every callback runs on a worker thread, gets its own
 * PGconn from the pool, and issues PQexecParams directly).
 * ------------------------------------------------------------------ */

/* Execute a query that returns no rows. Returns 0 or -EIO. */
static int exec_command(PGconn *pg, const char *sql,
                        int nparams, const char *const *params) {
    PGresult *r = PQexecParams(pg, sql, nparams, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
    if (!ok) trace("sql: %s", PQresultErrorMessage(r));
    PQclear(r);
    return ok ? 0 : -EIO;
}

/* Emit a NOTIFY for a parent path change. Used by every mutation path
 * inside the daemon (CLI-side mutations go through libviewfs's
 * vfs_notify_path). Failures are non-fatal: the 2-second timeout in the
 * kernel will eventually catch up. */
static void notify_parent(PGconn *pg, const char *parent_path) {
    char payload[VFS_PATH_MAX + 256];
    int n = snprintf(payload, sizeof payload, "%s\t%s",
                     CTX.view_name, parent_path ? parent_path : "");
    if (n < 0 || (size_t)n >= sizeof payload) return;
    const char *params[2] = { "viewfs_change", payload };
    PGresult *r = PQexecParams(pg, "SELECT pg_notify($1, $2)",
                               2, NULL, params, NULL, NULL, 0);
    PQclear(r);
}

/* Convenience snprintf for an int64_t into a small stack buffer. */
#define I64STR(name, val) char name[24]; snprintf(name, sizeof name, "%lld", (long long)(val))

/* Walk parent_path components and INSERT IF NOT EXISTS each prefix as
 * an explicit directory mapping. parent_path may be "" (root). */
static int ensure_parent_dirs_pg(PGconn *pg, const char *view,
                                 const char *parent_path) {
    if (!parent_path[0]) return 0;

    const char *p = parent_path + 1;  /* skip leading '/' */
    char accum[VFS_PATH_MAX];
    size_t alen = 0;
    int64_t now = vfs_now_ns();
    I64STR(now_s, now);

    while (1) {
        const char *slash = strchr(p, '/');
        size_t seglen = slash ? (size_t)(slash - p) : strlen(p);
        if (alen + 1 + seglen >= sizeof accum) return -EINVAL;
        accum[alen++] = '/';
        memcpy(accum + alen, p, seglen);
        alen += seglen;
        accum[alen] = '\0';

        const char *last_slash = strrchr(accum, '/');
        const char *parent_str = "";
        char parent_buf[VFS_PATH_MAX];
        if (last_slash && last_slash != accum) {
            size_t pl = (size_t)(last_slash - accum);
            memcpy(parent_buf, accum, pl);
            parent_buf[pl] = '\0';
            parent_str = parent_buf;
        }
        const char *name_str = last_slash ? last_slash + 1 : accum;

        const char *params[6] = { view, accum, parent_str, name_str, now_s, now_s };
        int rc = exec_command(pg,
            "INSERT INTO mappings "
            "(view_name, view_path, parent_path, name, entry_kind, object_id, "
            " ctime_ns, mtime_ns) "
            "VALUES ($1, $2, $3, $4, 'dir', NULL, $5::bigint, $6::bigint) "
            "ON CONFLICT (view_name, view_path) DO NOTHING",
            6, params);
        if (rc != 0) return rc;

        if (!slash) break;
        p = slash + 1;
        if (!*p) break;
    }
    return 0;
}

/* Try to load the persisted SHA-256 intermediate state for an object
 * into the given ofile and mark it sha_live, but only if the DB's
 * recorded size matches the current host-fd size — otherwise the state
 * doesn't match the bytes we'd be about to append to. */
static void try_load_checksum_state(PGconn *pg, const char *oid_hex,
                                    struct vfs_ofile *o) {
    struct stat st;
    if (fstat(o->fd, &st) != 0) return;
    const char *params[1] = { oid_hex };
    PGresult *r = PQexecParams(pg,
        "SELECT size, encode(checksum_state, 'hex') "
        "FROM objects WHERE object_id = $1",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return; }
    if (PQntuples(r) != 1)                    { PQclear(r); return; }
    if (PQgetisnull(r, 0, 1))                 { PQclear(r); return; }
    int64_t db_size  = atoll(PQgetvalue(r, 0, 0));
    if (db_size != (int64_t)st.st_size)       { PQclear(r); return; }

    const char *hex = PQgetvalue(r, 0, 1);
    size_t hex_len = strlen(hex);
    if (hex_len != VFS_SHA256_STATE_LEN * 2)  { PQclear(r); return; }

    unsigned char raw[VFS_SHA256_STATE_LEN];
    for (size_t i = 0; i < VFS_SHA256_STATE_LEN; i++) {
        unsigned hi = hex[2*i], lo = hex[2*i + 1];
        hi = (hi >= 'a') ? (hi - 'a' + 10) : (hi - '0');
        lo = (lo >= 'a') ? (lo - 'a' + 10) : (lo - '0');
        raw[i] = (unsigned char)((hi << 4) | lo);
    }
    PQclear(r);

    memset(&o->sha, 0, sizeof o->sha);
    if (vfs_sha256_stream_restore(&o->sha, raw, sizeof raw) != VFS_OK) return;
    o->sha_live   = 1;
    o->known_size = db_size;
}

/* Look up a mapping by path. Returns 0 on hit, -ENOENT, -EIO, etc.
 * On hit, fills out_kind ("file"/"dir"/"symlink") and out_obj_id_hex
 * (empty for dirs). */
static int lookup_mapping(PGconn *pg, const char *view, const char *vpath,
                          char *out_kind, size_t kind_sz,
                          char *out_obj_id, size_t obj_id_sz) {
    const char *params[2] = { view, vpath };
    PGresult *r = PQexecParams(pg,
        "SELECT entry_kind, COALESCE(object_id, '') "
        "FROM mappings WHERE view_name = $1 AND view_path = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -EIO; }
    if (PQntuples(r) == 0) { PQclear(r); return -ENOENT; }
    snprintf(out_kind, kind_sz, "%s", PQgetvalue(r, 0, 0));
    snprintf(out_obj_id, obj_id_sz, "%s", PQgetvalue(r, 0, 1));
    PQclear(r);
    return 0;
}

/* ------------------------------------------------------------------
 * getattr / readdir / open / read / release
 * ------------------------------------------------------------------ */

static int op_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi) {
    (void)fi;
    trace("getattr %s", path);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) {
        fill_dir_stat(st, 0755, vfs_now_ns());
        return 0;
    }
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;

    const char *params[2] = { CTX.view_name, cp.path };
    PGresult *r = PQexecParams(pg,
        "SELECT m.entry_kind, m.mode_override, m.mtime_ns, "
        "       o.size, o.mode, o.ctime_ns, o.mtime_ns, o.atime_ns, "
        "       o.uid, o.gid "
        "FROM mappings m "
        "LEFT JOIN objects o ON o.object_id = m.object_id "
        "WHERE m.view_name = $1 AND m.view_path = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        trace("getattr SQL: %s", PQresultErrorMessage(r));
        PQclear(r);
        return -EIO;
    }
    if (PQntuples(r) == 0) { PQclear(r); return -ENOENT; }

    const char *kind = PQgetvalue(r, 0, 0);
    int uid = PQgetisnull(r, 0, 8) ? -1 : atoi(PQgetvalue(r, 0, 8));
    int gid = PQgetisnull(r, 0, 9) ? -1 : atoi(PQgetvalue(r, 0, 9));
    if (!strcmp(kind, "dir")) {
        int mode = PQgetisnull(r, 0, 1) ? 0 : atoi(PQgetvalue(r, 0, 1));
        int64_t mt = atoll(PQgetvalue(r, 0, 2));
        fill_dir_stat(st, mode, mt);
    } else if (!strcmp(kind, "file")) {
        int64_t size = atoll(PQgetvalue(r, 0, 3));
        int     mode = atoi (PQgetvalue(r, 0, 4));
        int64_t ctime_ns = atoll(PQgetvalue(r, 0, 5));
        int64_t mtime_ns = atoll(PQgetvalue(r, 0, 6));
        int64_t atime_ns = atoll(PQgetvalue(r, 0, 7));
        fill_file_stat(st, mode, size, uid, gid, ctime_ns, mtime_ns, atime_ns);
    } else if (!strcmp(kind, "symlink")) {
        /* Symlink. Re-query the object row for the target so we can fill
         * st_size with strlen(target) -- ls -l displays this and some
         * tools depend on it. */
        memset(st, 0, sizeof *st);
        st->st_mode  = S_IFLNK | 0777;
        st->st_nlink = 1;
        st->st_uid   = (uid >= 0) ? (uid_t)uid : getuid();
        st->st_gid   = (gid >= 0) ? (gid_t)gid : getgid();
        int64_t ctime_ns = atoll(PQgetvalue(r, 0, 5));
        int64_t mtime_ns = atoll(PQgetvalue(r, 0, 6));
        int64_t atime_ns = atoll(PQgetvalue(r, 0, 7));
        st->st_ctim.tv_sec  = ctime_ns / 1000000000;
        st->st_ctim.tv_nsec = ctime_ns % 1000000000;
        st->st_mtim.tv_sec  = mtime_ns / 1000000000;
        st->st_mtim.tv_nsec = mtime_ns % 1000000000;
        st->st_atim.tv_sec  = atime_ns / 1000000000;
        st->st_atim.tv_nsec = atime_ns % 1000000000;
        st->st_size = atoll(PQgetvalue(r, 0, 3));  /* persisted size of target */
        PQclear(r);
        return 0;
    } else {
        PQclear(r);
        return -ENOENT;
    }
    PQclear(r);
    return 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)off; (void)fi; (void)flags;
    trace("readdir %s", path);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;

    const char *parent_q;
    if (cp.is_root) {
        parent_q = "";
    } else {
        char kind[16], oid[VFS_OID_HEX_LEN + 1];
        PGconn *pg = conn_pool_get(CTX.pool);
        if (!pg) return -EIO;
        rc = lookup_mapping(pg, CTX.view_name, cp.path,
                            kind, sizeof kind, oid, sizeof oid);
        if (rc) return rc;
        if (strcmp(kind, "dir") != 0) return -ENOTDIR;
        parent_q = cp.path;
    }

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    const char *params[2] = { CTX.view_name, parent_q };
    PGresult *r = PQexecParams(pg,
        "SELECT name FROM mappings "
        "WHERE view_name = $1 AND parent_path = $2 ORDER BY name",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -EIO; }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        filler(buf, PQgetvalue(r, i, 0), NULL, 0, 0);
    }
    PQclear(r);
    return 0;
}

/* Open the content file for an existing file mapping. Caller has already
 * looked up object id. Returns the host fd or -errno. */
static int open_content_file(const vfs_object_id *id, int flags) {
    char path[VFS_PATH_MAX];
    if (vfs_content_path(CTX.store, id, path, sizeof path) != VFS_OK)
        return -EIO;
    int fd = open(path, flags | O_CLOEXEC);
    return (fd < 0) ? -errno : fd;
}

static int op_open(const char *path, struct fuse_file_info *fi) {
    trace("open %s flags=0x%x", path, fi->flags);
    int acc = fi->flags & O_ACCMODE;
    int wants_write = (acc != O_RDONLY);
    if (wants_write && CTX.read_only) return -EROFS;

    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_mapping(pg, CTX.view_name, cp.path,
                        kind, sizeof kind, oid, sizeof oid);
    if (rc) return rc;
    if (!strcmp(kind, "dir"))  return -EISDIR;
    if (strcmp(kind, "file"))  return -EINVAL;

    vfs_object_id id;
    snprintf(id.hex, sizeof id.hex, "%s", oid);

    /* Host open flags = caller's flags, but never O_CREAT/O_EXCL because the
     * file already exists in our store. O_TRUNC and O_APPEND are honored. */
    int host_flags = acc;
    if (fi->flags & O_TRUNC)  host_flags |= O_TRUNC;
    if (fi->flags & O_APPEND) host_flags |= O_APPEND;

    int fd = open_content_file(&id, host_flags);
    if (fd < 0) return fd;

    struct vfs_ofile *o = ofile_new(fd, wants_write);
    if (!o) { close(fd); return -ENOMEM; }
    /* For an existing file opened for writing, try to resume the
     * checksum stream from the DB. If the stored state's size doesn't
     * match the current on-disk size (or there's no state), sha_live
     * stays 0 and any write will null the checksum on close. */
    if (wants_write && !(fi->flags & O_TRUNC)) {
        try_load_checksum_state(pg, id.hex, o);
    }
    fi->fh = (uint64_t)(uintptr_t)o;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    int fd = VFS_OFILE(fi)->fd;
    ssize_t n;
    do { n = pread(fd, buf, size, off); }
    while (n < 0 && errno == EINTR);
    int rc = (n < 0) ? -errno : (int)n;
    trace("read %s sz=%zu off=%lld -> %d", path, size, (long long)off, rc);
    return rc;
}

static int op_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    struct vfs_ofile *o = VFS_OFILE(fi);
    ssize_t n;
    do { n = pwrite(o->fd, buf, size, off); }
    while (n < 0 && errno == EINTR);
    if (n > 0) {
        o->modified = 1;
        if (o->sha_live && (int64_t)off == o->known_size) {
            /* Pure append: extend the running hash in place. */
            if (vfs_sha256_stream_update(&o->sha, buf, (size_t)n) == VFS_OK) {
                o->known_size += n;
            } else {
                ofile_invalidate_sha(o);
            }
        } else if (o->sha_live) {
            /* Out-of-order or overwrite: hash state is no longer valid
             * for whatever the file content will end up being. */
            ofile_invalidate_sha(o);
        }
    }
    int rc = (n < 0) ? -errno : (int)n;
    trace("write %s sz=%zu off=%lld -> %d", path, size, (long long)off, rc);
    return rc;
}

/* Render `n` bytes as lowercase hex into out (which must hold 2*n+1). */
static void bytes_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]     = H[(in[i] >> 4) & 0xF];
        out[2*i + 1] = H[in[i] & 0xF];
    }
    out[2*n] = '\0';
}

/* Update size/mtime/atime, and (when ofile is non-NULL) the checksum
 * columns too. If the per-fd SHA state is still live and covers the
 * full current file size, write the new digest + intermediate state.
 * Otherwise null both checksum columns — they're no longer trustworthy. */
static int sync_object_meta(PGconn *pg, const char *oid_hex,
                            struct vfs_ofile *o, int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -errno;
    I64STR(size_s,  (int64_t)st.st_size);
    int64_t mt_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    int64_t at_ns = (int64_t)st.st_atim.tv_sec * 1000000000LL + st.st_atim.tv_nsec;
    I64STR(mt_s, mt_ns);
    I64STR(at_s, at_ns);

    int can_hash = o && o->sha_live && o->known_size == (int64_t)st.st_size;
    if (can_hash) {
        unsigned char state[VFS_SHA256_STATE_LEN];
        char state_hex[VFS_SHA256_STATE_LEN * 2 + 1];
        char hex[65];
        if (vfs_sha256_stream_snapshot(&o->sha, state) != VFS_OK ||
            vfs_sha256_stream_finalize(&o->sha, hex)   != VFS_OK) {
            ofile_invalidate_sha(o);
            can_hash = 0;
        } else {
            o->sha_live = 0;  /* finalize consumed the ctx */
            bytes_to_hex(state, sizeof state, state_hex);
            const char *params[6] = { size_s, mt_s, at_s, hex, state_hex, oid_hex };
            return exec_command(pg,
                "UPDATE objects "
                "SET size = $1::bigint, mtime_ns = $2::bigint, "
                "    atime_ns = $3::bigint, checksum = $4, "
                "    checksum_state = decode($5, 'hex') "
                "WHERE object_id = $6",
                6, params);
        }
    }
    /* sha_live was false (or finalize failed): null both columns. */
    const char *params[4] = { size_s, mt_s, at_s, oid_hex };
    return exec_command(pg,
        "UPDATE objects "
        "SET size = $1::bigint, mtime_ns = $2::bigint, "
        "    atime_ns = $3::bigint, checksum = NULL, "
        "    checksum_state = NULL "
        "WHERE object_id = $4",
        4, params);
}

static int op_release(const char *path, struct fuse_file_info *fi) {
    struct vfs_ofile *o = VFS_OFILE(fi);
    trace("release %s fd=%d", path, o ? o->fd : -1);
    ofile_free(o);
    fi->fh = 0;
    return 0;
}

/* ------------------------------------------------------------------
 * create / truncate / unlink / mkdir / rmdir
 * ------------------------------------------------------------------ */

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    trace("create %s mode=%04o", path, mode);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EISDIR;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;

    vfs_object_id id;
    if (vfs_object_id_generate(&id) != VFS_OK) return -EIO;
    int64_t now = vfs_now_ns();
    I64STR(now_s, now);
    I64STR(mode_s, (int64_t)(mode & 07777));

    /* Phase 9 ordering: create the content file (and fsync it + its
     * parent dir) BEFORE the metadata transaction commits. A crash
     * between content creation and DB commit leaves an orphan content
     * file -- recoverable via `viewfs check --fix`. A crash after the
     * commit leaves a fully consistent new object. */
    if (vfs_content_create_empty(CTX.store, &id) != VFS_OK) return -EIO;

    if (exec_command(pg, "BEGIN", 0, NULL)) {
        vfs_content_unlink(CTX.store, &id);
        return -EIO;
    }
    rc = ensure_parent_dirs_pg(pg, CTX.view_name, cp.parent);
    if (rc) {
        exec_command(pg, "ROLLBACK", 0, NULL);
        vfs_content_unlink(CTX.store, &id);
        return rc;
    }

    /* Initialize a SHA stream for the new empty file; its initial hex
     * + intermediate state go straight into the row, and the same
     * stream is kept live in the ofile for subsequent appends. */
    vfs_sha256_stream sha = { 0 };
    if (vfs_sha256_stream_init(&sha) != VFS_OK) {
        exec_command(pg, "ROLLBACK", 0, NULL);
        vfs_content_unlink(CTX.store, &id);
        return -EIO;
    }
    unsigned char state_raw[VFS_SHA256_STATE_LEN];
    char state_hex[VFS_SHA256_STATE_LEN * 2 + 1];
    char hex[65];
    if (vfs_sha256_stream_snapshot(&sha, state_raw) != VFS_OK ||
        vfs_sha256_stream_peek_hex(&sha, hex)       != VFS_OK) {
        vfs_sha256_stream_abort(&sha);
        exec_command(pg, "ROLLBACK", 0, NULL);
        vfs_content_unlink(CTX.store, &id);
        return -EIO;
    }
    bytes_to_hex(state_raw, sizeof state_raw, state_hex);

    struct fuse_context *fctx = fuse_get_context();
    I64STR(uid_s, (int64_t)fctx->uid);
    I64STR(gid_s, (int64_t)fctx->gid);

    const char *oparams[9] = {
        id.hex, mode_s, now_s, now_s, now_s, uid_s, gid_s, hex, state_hex
    };
    rc = exec_command(pg,
        "INSERT INTO objects "
        "(object_id, kind, mode, size, ctime_ns, mtime_ns, atime_ns, "
        " uid, gid, checksum, checksum_state) "
        "VALUES ($1, 'file', $2::int, 0, $3::bigint, $4::bigint, "
        "        $5::bigint, $6::int, $7::int, $8, decode($9, 'hex'))",
        9, oparams);
    if (rc) {
        vfs_sha256_stream_abort(&sha);
        exec_command(pg, "ROLLBACK", 0, NULL);
        vfs_content_unlink(CTX.store, &id);
        return rc;
    }

    const char *mparams[7] = {
        CTX.view_name, cp.path, cp.parent, cp.name, id.hex, now_s, now_s
    };
    rc = exec_command(pg,
        "INSERT INTO mappings "
        "(view_name, view_path, parent_path, name, entry_kind, object_id, "
        " ctime_ns, mtime_ns) "
        "VALUES ($1, $2, $3, $4, 'file', $5, $6::bigint, $7::bigint)",
        7, mparams);
    if (rc) {
        vfs_sha256_stream_abort(&sha);
        exec_command(pg, "ROLLBACK", 0, NULL);
        vfs_content_unlink(CTX.store, &id);
        return rc;
    }

    if (exec_command(pg, "COMMIT", 0, NULL)) {
        vfs_sha256_stream_abort(&sha);
        exec_command(pg, "ROLLBACK", 0, NULL);
        vfs_content_unlink(CTX.store, &id);
        return -EIO;
    }
    notify_parent(pg, cp.parent);
    if (cp.parent[0]) notify_parent(pg, "");
    int fd = open_content_file(&id, O_RDWR);
    if (fd < 0) {
        /* metadata committed and content exists, but we somehow can't
         * open it -- caller will see -EIO and `viewfs check` would
         * report any mismatch. Best to NOT unlink here lest we orphan
         * a committed mapping. */
        vfs_sha256_stream_abort(&sha);
        return fd;
    }
    struct vfs_ofile *o = ofile_new(fd, 1);
    if (!o) { vfs_sha256_stream_abort(&sha); close(fd); return -ENOMEM; }
    o->sha        = sha;     /* hand the live stream to the ofile */
    o->sha_live   = 1;
    o->known_size = 0;
    fi->fh = (uint64_t)(uintptr_t)o;
    return 0;
}

static int op_truncate(const char *path, off_t off, struct fuse_file_info *fi) {
    trace("truncate %s off=%lld", path, (long long)off);
    if (CTX.read_only) return -EROFS;
    if (fi && fi->fh) {
        struct vfs_ofile *o = VFS_OFILE(fi);
        if (ftruncate(o->fd, off) != 0) return -errno;
        /* Any truncate (grow OR shrink) invalidates the running hash:
         * shrinking removes already-hashed bytes; growing inserts zero
         * bytes between known_size and the new EOF that we never fed
         * to the stream. Either way the SHA is no longer trustworthy. */
        o->modified = 1;
        ofile_invalidate_sha(o);
        return 0;
    }
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_mapping(pg, CTX.view_name, cp.path,
                        kind, sizeof kind, oid, sizeof oid);
    if (rc) return rc;
    if (strcmp(kind, "file")) return -EISDIR;
    vfs_object_id id;
    snprintf(id.hex, sizeof id.hex, "%s", oid);
    int fd = open_content_file(&id, O_RDWR);
    if (fd < 0) return fd;
    int rc2 = (ftruncate(fd, off) != 0) ? -errno : 0;
    if (rc2 == 0) {
        sync_object_meta(pg, oid, NULL, fd);
        notify_parent(pg, cp.parent);
    }
    close(fd);
    return rc2;
}

static int op_unlink(const char *path) {
    trace("unlink %s", path);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EISDIR;
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    const char *params[2] = { CTX.view_name, cp.path };
    PGresult *r = PQexecParams(pg,
        "DELETE FROM mappings WHERE view_name=$1 AND view_path=$2 "
        "AND entry_kind <> 'dir'",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        trace("unlink sql: %s", PQresultErrorMessage(r));
        PQclear(r); return -EIO;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    if (!affected) {
        /* could be a directory or non-existent — check */
        char kind[16], oid[VFS_OID_HEX_LEN + 1];
        rc = lookup_mapping(pg, CTX.view_name, cp.path, kind, sizeof kind,
                            oid, sizeof oid);
        if (rc == -ENOENT) return -ENOENT;
        if (!strcmp(kind, "dir")) return -EISDIR;
        return -EIO;
    }
    notify_parent(pg, cp.parent);
    return 0;
}

static int op_mkdir(const char *path, mode_t mode) {
    trace("mkdir %s mode=%04o", path, mode);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EEXIST;
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    if (exec_command(pg, "BEGIN", 0, NULL)) return -EIO;
    rc = ensure_parent_dirs_pg(pg, CTX.view_name, cp.parent);
    if (rc) { exec_command(pg, "ROLLBACK", 0, NULL); return rc; }

    int64_t now = vfs_now_ns();
    I64STR(now_s, now);
    I64STR(mode_s, (int64_t)(mode & 07777));
    const char *params[7] = {
        CTX.view_name, cp.path, cp.parent, cp.name, mode_s, now_s, now_s
    };
    PGresult *r = PQexecParams(pg,
        "INSERT INTO mappings "
        "(view_name, view_path, parent_path, name, entry_kind, object_id, "
        " mode_override, ctime_ns, mtime_ns) "
        "VALUES ($1, $2, $3, $4, 'dir', NULL, $5::int, $6::bigint, $7::bigint)",
        7, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
    const char *code = ok ? NULL : PQresultErrorField(r, PG_DIAG_SQLSTATE);
    int already = (code && !strcmp(code, "23505"));
    if (!ok) trace("mkdir sql: %s", PQresultErrorMessage(r));
    PQclear(r);
    if (!ok) {
        exec_command(pg, "ROLLBACK", 0, NULL);
        return already ? -EEXIST : -EIO;
    }
    if (exec_command(pg, "COMMIT", 0, NULL)) return -EIO;
    notify_parent(pg, cp.parent);
    if (cp.parent[0]) notify_parent(pg, "");
    return 0;
}

static int op_rmdir(const char *path) {
    trace("rmdir %s", path);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EBUSY;
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;

    /* Check existence + emptiness in one query. */
    const char *params[2] = { CTX.view_name, cp.path };
    PGresult *r = PQexecParams(pg,
        "SELECT entry_kind, "
        "  (SELECT count(*) FROM mappings c "
        "   WHERE c.view_name = $1 AND c.parent_path = $2) AS n "
        "FROM mappings WHERE view_name = $1 AND view_path = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -EIO; }
    if (PQntuples(r) == 0) { PQclear(r); return -ENOENT; }
    const char *kind = PQgetvalue(r, 0, 0);
    int children = atoi(PQgetvalue(r, 0, 1));
    if (strcmp(kind, "dir") != 0) { PQclear(r); return -ENOTDIR; }
    if (children > 0)             { PQclear(r); return -ENOTEMPTY; }
    PQclear(r);

    int rc2 = exec_command(pg,
        "DELETE FROM mappings WHERE view_name = $1 AND view_path = $2",
        2, params);
    if (rc2 == 0) notify_parent(pg, cp.parent);
    return rc2 == 0 ? 0 : -EIO;
}

/* ------------------------------------------------------------------
 * symlinks
 * ------------------------------------------------------------------ */

/* Reject targets with control chars; otherwise treat as opaque strings.
 * The README documents that kernel-side resolution of symlinks is outside
 * our control: an absolute target like "/etc/passwd" escapes the view at
 * the kernel layer, just as it does in any FUSE filesystem. Users who
 * want strict isolation should pair ViewFS with mount namespaces. */
static int validate_symlink_target(const char *target) {
    if (!target || !*target) return -EINVAL;
    if (strlen(target) >= VFS_PATH_MAX) return -ENAMETOOLONG;
    for (const unsigned char *p = (const unsigned char*)target; *p; p++) {
        if (*p < 0x20) return -EINVAL;
    }
    return 0;
}

static int op_symlink(const char *target, const char *linkpath) {
    trace("symlink %s -> %s", linkpath, target);
    if (CTX.read_only) return -EROFS;

    int trc = validate_symlink_target(target);
    if (trc) return trc;

    vfs_canon_path cp;
    int rc = canon(linkpath, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EEXIST;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;

    vfs_object_id id;
    if (vfs_object_id_generate(&id) != VFS_OK) return -EIO;
    int64_t now = vfs_now_ns();
    I64STR(now_s, now);
    I64STR(size_s, (int64_t)strlen(target));

    if (exec_command(pg, "BEGIN", 0, NULL)) return -EIO;
    rc = ensure_parent_dirs_pg(pg, CTX.view_name, cp.parent);
    if (rc) { exec_command(pg, "ROLLBACK", 0, NULL); return rc; }

    struct fuse_context *fctx = fuse_get_context();
    I64STR(uid_s, (int64_t)fctx->uid);
    I64STR(gid_s, (int64_t)fctx->gid);
    const char *oparams[8] = {
        id.hex, size_s, now_s, now_s, now_s, target, uid_s, gid_s
    };
    rc = exec_command(pg,
        "INSERT INTO objects "
        "(object_id, kind, mode, size, ctime_ns, mtime_ns, atime_ns, "
        " symlink_target, uid, gid) "
        "VALUES ($1, 'symlink', 0777, $2::bigint, $3::bigint, $4::bigint, "
        "        $5::bigint, $6, $7::int, $8::int)",
        8, oparams);
    if (rc) { exec_command(pg, "ROLLBACK", 0, NULL); return rc; }

    const char *mparams[7] = {
        CTX.view_name, cp.path, cp.parent, cp.name, id.hex, now_s, now_s
    };
    rc = exec_command(pg,
        "INSERT INTO mappings "
        "(view_name, view_path, parent_path, name, entry_kind, object_id, "
        " ctime_ns, mtime_ns) "
        "VALUES ($1, $2, $3, $4, 'symlink', $5, $6::bigint, $7::bigint)",
        7, mparams);
    if (rc) { exec_command(pg, "ROLLBACK", 0, NULL); return rc; }

    if (exec_command(pg, "COMMIT", 0, NULL)) {
        exec_command(pg, "ROLLBACK", 0, NULL);
        return -EIO;
    }
    notify_parent(pg, cp.parent);
    if (cp.parent[0]) notify_parent(pg, "");
    return 0;
}

static int op_readlink(const char *path, char *buf, size_t size) {
    trace("readlink %s", path);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EINVAL;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    const char *params[2] = { CTX.view_name, cp.path };
    PGresult *r = PQexecParams(pg,
        "SELECT o.symlink_target FROM mappings m "
        "JOIN objects o ON o.object_id = m.object_id "
        "WHERE m.view_name = $1 AND m.view_path = $2 "
        "AND m.entry_kind = 'symlink'",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -EIO; }
    if (PQntuples(r) == 0)                    { PQclear(r); return -EINVAL; }
    if (PQgetisnull(r, 0, 0))                 { PQclear(r); return -EIO; }
    const char *target = PQgetvalue(r, 0, 0);
    size_t tlen = strlen(target);
    if (size == 0)            { PQclear(r); return -EINVAL; }
    if (tlen >= size) tlen = size - 1;
    memcpy(buf, target, tlen);
    buf[tlen] = '\0';
    PQclear(r);
    return 0;
}

/* ------------------------------------------------------------------
 * rename (intra-mount only; cross-mount returns EXDEV via kernel)
 * ------------------------------------------------------------------ */

static int op_rename(const char *from, const char *to, unsigned int flags) {
    trace("rename %s -> %s flags=0x%x", from, to, flags);
    if (CTX.read_only) return -EROFS;
    if (flags & RENAME_EXCHANGE) return -EINVAL;
    if (flags & ~((unsigned)RENAME_NOREPLACE)) return -EINVAL;

    vfs_canon_path sf, df;
    int rc = canon(from, &sf); if (rc) return rc;
    rc = canon(to,   &df);     if (rc) return rc;
    if (sf.is_root || df.is_root) return -EBUSY;
    if (!strcmp(sf.path, df.path)) return 0;

    /* Refuse moving a directory into its own descendant. */
    size_t slen = strlen(sf.path);
    if (!strncmp(df.path, sf.path, slen) && df.path[slen] == '/')
        return -EINVAL;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    if (exec_command(pg, "BEGIN", 0, NULL)) return -EIO;

    /* Confirm source exists; cache its kind. */
    char skind[16], soid[VFS_OID_HEX_LEN + 1];
    rc = lookup_mapping(pg, CTX.view_name, sf.path, skind, sizeof skind,
                        soid, sizeof soid);
    if (rc) { exec_command(pg, "ROLLBACK", 0, NULL); return rc; }

    /* Check destination state. */
    char dkind[16], doid[VFS_OID_HEX_LEN + 1];
    int drc = lookup_mapping(pg, CTX.view_name, df.path, dkind, sizeof dkind,
                             doid, sizeof doid);
    if (drc == 0) {
        if (flags & RENAME_NOREPLACE) {
            exec_command(pg, "ROLLBACK", 0, NULL);
            return -EEXIST;
        }
        /* If dest is a dir, source must also be a dir AND dest must be empty. */
        if (!strcmp(dkind, "dir")) {
            if (strcmp(skind, "dir")) {
                exec_command(pg, "ROLLBACK", 0, NULL);
                return -EISDIR;
            }
            const char *cparams[2] = { CTX.view_name, df.path };
            PGresult *cr = PQexecParams(pg,
                "SELECT count(*) FROM mappings "
                "WHERE view_name = $1 AND parent_path = $2",
                2, NULL, cparams, NULL, NULL, 0);
            int n = (PQresultStatus(cr) == PGRES_TUPLES_OK)
                ? atoi(PQgetvalue(cr, 0, 0)) : -1;
            PQclear(cr);
            if (n != 0) {
                exec_command(pg, "ROLLBACK", 0, NULL);
                return n < 0 ? -EIO : -ENOTEMPTY;
            }
        } else if (!strcmp(skind, "dir")) {
            exec_command(pg, "ROLLBACK", 0, NULL);
            return -ENOTDIR;
        }
        /* Delete the existing destination mapping. */
        const char *dp[2] = { CTX.view_name, df.path };
        if (exec_command(pg,
            "DELETE FROM mappings WHERE view_name = $1 AND view_path = $2",
            2, dp)) { exec_command(pg, "ROLLBACK", 0, NULL); return -EIO; }
    } else if (drc != -ENOENT) {
        exec_command(pg, "ROLLBACK", 0, NULL);
        return drc;
    }

    rc = ensure_parent_dirs_pg(pg, CTX.view_name, df.parent);
    if (rc) { exec_command(pg, "ROLLBACK", 0, NULL); return rc; }

    /* If source is a directory, rewrite the prefixes of every descendant
     * BEFORE moving the directory itself, so the PK on the moved row
     * doesn't collide with descendants. */
    if (!strcmp(skind, "dir")) {
        const char *p1[4] = { CTX.view_name, sf.path, df.path, sf.path };
        if (exec_command(pg,
            "UPDATE mappings SET "
            "  view_path   = $3 || substr(view_path,   length($2) + 1), "
            "  parent_path = $3 || substr(parent_path, length($2) + 1) "
            "WHERE view_name = $1 AND view_path LIKE $4 || '/%'",
            4, p1)) { exec_command(pg, "ROLLBACK", 0, NULL); return -EIO; }
    }

    /* Move the entry itself: update view_path/parent_path/name. */
    const char *p2[5] = { CTX.view_name, sf.path, df.path, df.parent, df.name };
    if (exec_command(pg,
        "UPDATE mappings SET view_path = $3, parent_path = $4, name = $5 "
        "WHERE view_name = $1 AND view_path = $2",
        5, p2)) { exec_command(pg, "ROLLBACK", 0, NULL); return -EIO; }

    if (exec_command(pg, "COMMIT", 0, NULL)) {
        exec_command(pg, "ROLLBACK", 0, NULL); return -EIO;
    }
    notify_parent(pg, sf.parent);
    if (strcmp(sf.parent, df.parent) != 0) notify_parent(pg, df.parent);
    return 0;
}

/* ------------------------------------------------------------------
 * utimens / chmod / chown / flush / fsync
 * ------------------------------------------------------------------ */

static int op_utimens(const char *path, const struct timespec ts[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    trace("utimens %s", path);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return 0;
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_mapping(pg, CTX.view_name, cp.path,
                        kind, sizeof kind, oid, sizeof oid);
    if (rc) return rc;

    int64_t now = vfs_now_ns();
    int64_t atime_ns = (ts[0].tv_nsec == UTIME_NOW || ts[0].tv_nsec == UTIME_OMIT)
        ? now
        : (int64_t)ts[0].tv_sec * 1000000000LL + ts[0].tv_nsec;
    int64_t mtime_ns = (ts[1].tv_nsec == UTIME_NOW || ts[1].tv_nsec == UTIME_OMIT)
        ? now
        : (int64_t)ts[1].tv_sec * 1000000000LL + ts[1].tv_nsec;

    int rc2 = 0;
    if (!strcmp(kind, "file") && oid[0]) {
        I64STR(at_s, atime_ns); I64STR(mt_s, mtime_ns);
        const char *p[3] = { at_s, mt_s, oid };
        rc2 = exec_command(pg,
            "UPDATE objects SET atime_ns = $1::bigint, mtime_ns = $2::bigint "
            "WHERE object_id = $3",
            3, p);
    } else if (!strcmp(kind, "dir")) {
        I64STR(mt_s, mtime_ns);
        const char *p[3] = { mt_s, CTX.view_name, cp.path };
        rc2 = exec_command(pg,
            "UPDATE mappings SET mtime_ns = $1::bigint "
            "WHERE view_name = $2 AND view_path = $3",
            3, p);
    }
    if (rc2 == 0) notify_parent(pg, cp.parent);
    return rc2 == 0 ? 0 : -EIO;
}

static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    trace("chmod %s mode=%04o", path, mode);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return 0;
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_mapping(pg, CTX.view_name, cp.path,
                        kind, sizeof kind, oid, sizeof oid);
    if (rc) return rc;

    I64STR(mode_s, (int64_t)(mode & 07777));
    int rc2 = 0;
    if (!strcmp(kind, "file") && oid[0]) {
        const char *p[2] = { mode_s, oid };
        rc2 = exec_command(pg,
            "UPDATE objects SET mode = $1::int WHERE object_id = $2",
            2, p);
    } else if (!strcmp(kind, "dir")) {
        const char *p[3] = { mode_s, CTX.view_name, cp.path };
        rc2 = exec_command(pg,
            "UPDATE mappings SET mode_override = $1::int "
            "WHERE view_name = $2 AND view_path = $3",
            3, p);
    }
    if (rc2 == 0) notify_parent(pg, cp.parent);
    return rc2 == 0 ? 0 : -EIO;
}

static int op_chown(const char *path, uid_t uid, gid_t gid,
                    struct fuse_file_info *fi) {
    (void)fi;
    trace("chown %s uid=%u gid=%u", path, (unsigned)uid, (unsigned)gid);
    if (CTX.read_only) return -EROFS;
    int change_uid = (uid != (uid_t)-1);
    int change_gid = (gid != (gid_t)-1);
    if (!change_uid && !change_gid) return 0;  /* nothing to do */

    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return 0;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_mapping(pg, CTX.view_name, cp.path,
                        kind, sizeof kind, oid, sizeof oid);
    if (rc) return rc;
    /* Directory mappings don't have an underlying object; chown on a
     * directory is silently accepted but not persisted. */
    if (!strcmp(kind, "dir") || !oid[0]) return 0;

    I64STR(uid_s, (int64_t)uid);
    I64STR(gid_s, (int64_t)gid);
    int rc2 = 0;
    if (change_uid && change_gid) {
        const char *p[3] = { uid_s, gid_s, oid };
        rc2 = exec_command(pg,
            "UPDATE objects SET uid = $1::int, gid = $2::int "
            "WHERE object_id = $3",
            3, p);
    } else if (change_uid) {
        const char *p[2] = { uid_s, oid };
        rc2 = exec_command(pg,
            "UPDATE objects SET uid = $1::int WHERE object_id = $2",
            2, p);
    } else {
        const char *p[2] = { gid_s, oid };
        rc2 = exec_command(pg,
            "UPDATE objects SET gid = $1::int WHERE object_id = $2",
            2, p);
    }
    if (rc2 == 0) notify_parent(pg, cp.parent);
    return rc2 == 0 ? 0 : -EIO;
}

static int op_flush(const char *path, struct fuse_file_info *fi) {
    struct vfs_ofile *o = VFS_OFILE(fi);
    int fd        = o ? o->fd       : -1;
    int writable  = o ? o->writable : 0;
    int modified  = o ? o->modified : 0;
    trace("flush %s fd=%d write=%d modified=%d", path, fd, writable, modified);

    /* op_flush is what close(2) synchronizes against. By doing the fsync
     * here (rather than in op_release, which is async to close), a
     * close() that returns 0 implies the bytes are durably on disk and
     * the DB has recorded the new size + checksum. */
    if (!writable || !modified || fd <= 0 || !path) return 0;

    if (fsync(fd) != 0) return -errno;

    vfs_canon_path cp;
    if (canon(path, &cp) != 0 || cp.is_root) return 0;
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    if (lookup_mapping(pg, CTX.view_name, cp.path,
                       kind, sizeof kind, oid, sizeof oid) != 0
        || strcmp(kind, "file") != 0 || !oid[0]) {
        /* mapping vanished (e.g. unlinked since open) — nothing to record */
        o->modified = 0;
        return 0;
    }
    if (sync_object_meta(pg, oid, o, fd) != 0) return -EIO;
    notify_parent(pg, cp.parent);
    /* Idempotency: a second op_flush on this fd without further writes
     * should be a no-op. The sha was consumed (sha_live=0); resetting
     * modified prevents us from re-nulling the checksum we just set. */
    o->modified = 0;
    return 0;
}

static int op_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    struct vfs_ofile *o = VFS_OFILE(fi);
    int fd = o ? o->fd : -1;
    if (fd <= 0) { trace("fsync %s -> 0 (no fd)", path); return 0; }
    int rc = datasync ? fdatasync(fd) : fsync(fd);
    int ret = rc == 0 ? 0 : -errno;
    trace("fsync %s datasync=%d -> %d", path, datasync, ret);
    return ret;
}

/* ------------------------------------------------------------------
 * Extended attributes (Phase 5)
 *
 * Only names beginning with "user.viewfs." are honored; everything else
 * returns -ENOTSUP. The trailing portion is stored in the `attributes`
 * table keyed by the underlying object id.
 * ------------------------------------------------------------------ */

#include <sys/xattr.h>          /* XATTR_CREATE / XATTR_REPLACE */

static const char XATTR_PREFIX[] = "user.viewfs.";
#define XATTR_PREFIX_LEN  (sizeof XATTR_PREFIX - 1)

/* Resolve a path to its object id. Returns 0 on success, -errno on
 * failure. Directories use VFS_OID_HEX_LEN+1 of zeros (caller should
 * treat dir xattrs as not-yet-supported and return -ENOTSUP). */
static int lookup_object(PGconn *pg, const char *vpath,
                         char out_kind[16],
                         char out_oid[VFS_OID_HEX_LEN + 1]) {
    return lookup_mapping(pg, CTX.view_name, vpath,
                          out_kind, 16, out_oid, VFS_OID_HEX_LEN + 1);
}

static int op_getxattr(const char *path, const char *name,
                       char *value, size_t size) {
    trace("getxattr %s name=%s sz=%zu", path, name, size);
    if (strncmp(name, XATTR_PREFIX, XATTR_PREFIX_LEN) != 0) return -ENOTSUP;
    const char *key = name + XATTR_PREFIX_LEN;

    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -ENODATA;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_object(pg, cp.path, kind, oid);
    if (rc) return rc;
    if (!oid[0]) return -ENODATA;  /* directory mapping carries no object */

    const char *params[2] = { oid, key };
    PGresult *r = PQexecParams(pg,
        "SELECT value FROM attributes WHERE object_id = $1 AND key = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -EIO; }
    if (PQntuples(r) == 0) { PQclear(r); return -ENODATA; }
    const char *val = PQgetvalue(r, 0, 0);
    size_t vlen = (size_t)PQgetlength(r, 0, 0);
    int ret;
    if (size == 0) {
        ret = (int)vlen;
    } else if (size < vlen) {
        ret = -ERANGE;
    } else {
        memcpy(value, val, vlen);
        ret = (int)vlen;
    }
    PQclear(r);
    return ret;
}

static int op_setxattr(const char *path, const char *name,
                       const char *value, size_t size, int flags) {
    trace("setxattr %s name=%s sz=%zu flags=0x%x", path, name, size, flags);
    if (CTX.read_only) return -EROFS;
    if (strncmp(name, XATTR_PREFIX, XATTR_PREFIX_LEN) != 0) return -ENOTSUP;
    const char *key = name + XATTR_PREFIX_LEN;
    if (!*key) return -EINVAL;

    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EINVAL;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_object(pg, cp.path, kind, oid);
    if (rc) return rc;
    if (!oid[0]) return -ENOTSUP;  /* xattrs on directories deferred */

    /* The attributes.value column is TEXT. We pass the buffer with an
     * explicit length so embedded NULs would survive in principle, but
     * Postgres TEXT cannot contain '\0' -- reject early to avoid a DB
     * error. */
    for (size_t i = 0; i < size; i++) {
        if (value[i] == '\0') return -EINVAL;
    }

    int64_t now = vfs_now_ns();
    I64STR(now_s, now);

    /* The libpq client truncates at the first NUL anyway; size has been
     * validated to be NUL-free, so a null-terminated copy is safe. */
    char *vcopy = malloc(size + 1);
    if (!vcopy) return -ENOMEM;
    memcpy(vcopy, value, size);
    vcopy[size] = '\0';

    int ret;
    if (flags & XATTR_CREATE) {
        const char *params[5] = { oid, key, vcopy, now_s, now_s };
        PGresult *r = PQexecParams(pg,
            "INSERT INTO attributes (object_id, key, value, ctime_ns, mtime_ns) "
            "VALUES ($1, $2, $3, $4::bigint, $5::bigint) "
            "ON CONFLICT (object_id, key) DO NOTHING",
            5, NULL, params, NULL, NULL, 0);
        int ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
        int affected = ok ? atoi(PQcmdTuples(r)) : 0;
        PQclear(r);
        if (!ok) ret = -EIO;
        else if (!affected) ret = -EEXIST;
        else ret = 0;
    } else if (flags & XATTR_REPLACE) {
        const char *params[4] = { vcopy, now_s, oid, key };
        PGresult *r = PQexecParams(pg,
            "UPDATE attributes SET value = $1, mtime_ns = $2::bigint "
            "WHERE object_id = $3 AND key = $4",
            4, NULL, params, NULL, NULL, 0);
        int ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
        int affected = ok ? atoi(PQcmdTuples(r)) : 0;
        PQclear(r);
        if (!ok) ret = -EIO;
        else if (!affected) ret = -ENODATA;
        else ret = 0;
    } else {
        const char *params[5] = { oid, key, vcopy, now_s, now_s };
        PGresult *r = PQexecParams(pg,
            "INSERT INTO attributes (object_id, key, value, ctime_ns, mtime_ns) "
            "VALUES ($1, $2, $3, $4::bigint, $5::bigint) "
            "ON CONFLICT (object_id, key) DO UPDATE "
            "  SET value = EXCLUDED.value, mtime_ns = EXCLUDED.mtime_ns",
            5, NULL, params, NULL, NULL, 0);
        int ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
        PQclear(r);
        ret = ok ? 0 : -EIO;
    }
    free(vcopy);
    return ret;
}

static int op_listxattr(const char *path, char *list, size_t size) {
    trace("listxattr %s sz=%zu", path, size);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return 0;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_object(pg, cp.path, kind, oid);
    if (rc) return rc;
    if (!oid[0]) return 0;

    const char *params[1] = { oid };
    PGresult *r = PQexecParams(pg,
        "SELECT key FROM attributes WHERE object_id = $1 ORDER BY key",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -EIO; }

    /* Compute total bytes: sum("user.viewfs.<key>\0") for each row. */
    int n = PQntuples(r);
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        total += XATTR_PREFIX_LEN + (size_t)PQgetlength(r, i, 0) + 1;
    }
    if (size == 0) { PQclear(r); return (int)total; }
    if (size < total) { PQclear(r); return -ERANGE; }

    size_t off = 0;
    for (int i = 0; i < n; i++) {
        memcpy(list + off, XATTR_PREFIX, XATTR_PREFIX_LEN);
        off += XATTR_PREFIX_LEN;
        size_t kl = (size_t)PQgetlength(r, i, 0);
        memcpy(list + off, PQgetvalue(r, i, 0), kl);
        off += kl;
        list[off++] = '\0';
    }
    PQclear(r);
    return (int)total;
}

static int op_removexattr(const char *path, const char *name) {
    trace("removexattr %s name=%s", path, name);
    if (CTX.read_only) return -EROFS;
    if (strncmp(name, XATTR_PREFIX, XATTR_PREFIX_LEN) != 0) return -ENOTSUP;
    const char *key = name + XATTR_PREFIX_LEN;

    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -ENODATA;

    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    char kind[16], oid[VFS_OID_HEX_LEN + 1];
    rc = lookup_object(pg, cp.path, kind, oid);
    if (rc) return rc;
    if (!oid[0]) return -ENODATA;

    const char *params[2] = { oid, key };
    PGresult *r = PQexecParams(pg,
        "DELETE FROM attributes WHERE object_id = $1 AND key = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) { PQclear(r); return -EIO; }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    return affected ? 0 : -ENODATA;
}

/* ------------------------------------------------------------------
 * statfs / access / init / destroy (unchanged from Phase 2)
 * ------------------------------------------------------------------ */

static int op_statfs(const char *path, struct statvfs *st) {
    trace("statfs %s", path);
    if (statvfs(vfs_store_path(CTX.store), st) != 0) return -errno;
    return 0;
}

static int op_access(const char *path, int mask) {
    trace("access %s mask=0x%x", path, mask);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return 0;
    PGconn *pg = conn_pool_get(CTX.pool);
    if (!pg) return -EIO;
    const char *params[2] = { CTX.view_name, cp.path };
    PGresult *r = PQexecParams(pg,
        "SELECT 1 FROM mappings WHERE view_name=$1 AND view_path=$2",
        2, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(r) == PGRES_TUPLES_OK) && PQntuples(r) > 0;
    PQclear(r);
    if (!ok) return -ENOENT;
    if (CTX.read_only && (mask & W_OK)) return -EROFS;
    return 0;
}

static void pid_path(char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "%s/daemons/%s.pid",
             vfs_store_path(CTX.store), CTX.view_name);
}

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->use_ino = 0;
    /* Phase 4: short FUSE-side caches paired with LISTEN/NOTIFY-driven
     * invalidation. Mutations elsewhere (CLI commands, other daemons)
     * emit NOTIFY viewfs_change; the notify_loop calls
     * fuse_invalidate_path() to drop the kernel's dentry cache for the
     * affected directory. The 2-second timeout is a safety net for any
     * notification that gets dropped. */
    cfg->attr_timeout     = 2.0;
    cfg->entry_timeout    = 2.0;
    cfg->negative_timeout = 1.0;

    CTX.fuse_handle = fuse_get_context()->fuse;

    char p[VFS_PATH_MAX];
    pid_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }

    if (notify_thread_start(&CTX) != 0) {
        fprintf(stderr,
            "viewfs-fuse: notify thread failed to start; mount continues "
            "with cache disabled\n");
        cfg->attr_timeout = cfg->entry_timeout = cfg->negative_timeout = 0.0;
    }

    trace("mounted view='%s' ro=%d", CTX.view_name, CTX.read_only);
    return NULL;
}

static void op_destroy(void *priv) {
    (void)priv;
    notify_thread_stop();
    char p[VFS_PATH_MAX];
    pid_path(p, sizeof p);
    unlink(p);
    trace("unmounted");
}

const struct fuse_operations viewfs_oper = {
    .getattr     = op_getattr,
    .readdir     = op_readdir,
    .open        = op_open,
    .read        = op_read,
    .write       = op_write,
    .create      = op_create,
    .truncate    = op_truncate,
    .unlink      = op_unlink,
    .mkdir       = op_mkdir,
    .rmdir       = op_rmdir,
    .rename      = op_rename,
    .utimens     = op_utimens,
    .chmod       = op_chmod,
    .chown       = op_chown,
    .flush       = op_flush,
    .fsync       = op_fsync,
    .release     = op_release,
    .statfs      = op_statfs,
    .access      = op_access,
    .symlink     = op_symlink,
    .readlink    = op_readlink,
    .listxattr   = op_listxattr,
    .getxattr    = op_getxattr,
    .setxattr    = op_setxattr,
    .removexattr = op_removexattr,
    .init        = op_init,
    .destroy     = op_destroy,
};
