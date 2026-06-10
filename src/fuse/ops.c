#define _FILE_OFFSET_BITS 64

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>     /* RENAME_NOREPLACE, RENAME_EXCHANGE */
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/xattr.h>    /* XATTR_CREATE / XATTR_REPLACE */
#include <time.h>
#include <unistd.h>

#include "ops.h"
#include "notify.h"

viewfs_ctx CTX;

/* The property-model membership logic lives in libviewfs and operates on a
 * single (non-thread-safe) PGconn inside CTX.store. Rather than reimplement
 * the relational-division membership query against the per-thread conn pool,
 * the daemon routes all metadata through CTX.store under this mutex. Content
 * read/write (the hot path) touch only per-open file descriptors and never
 * take the lock. For a single-host prototype this trades some metadata
 * concurrency for one source of truth. */
static pthread_mutex_t g_db = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_db)
#define UNLOCK() pthread_mutex_unlock(&g_db)

/* Per-open-file state; fi->fh holds a pointer to one of these. */
struct vfs_ofile {
    int            fd;
    int            writable;
    int            modified;          /* >=1 write/truncate since open */
    vfs_object_id  id;                /* the backing object */
    char           parent[VFS_PATH_MAX];  /* parent dir, for NOTIFY */
};
#define VFS_OFILE(fi)  ((struct vfs_ofile *)(uintptr_t)(fi)->fh)

static void trace(const char *fmt, ...) {
    if (!CTX.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "vfs-fuse[%s]: ", CTX.view_name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Canonicalize a FUSE-supplied path into cp, returning -errno on bad input. */
static int canon(const char *in, vfs_canon_path *out) {
    switch (vfs_path_canonicalize(in, out)) {
    case VFS_OK:                return 0;
    case VFS_ERR_PATH_RELATIVE: return -EINVAL;
    case VFS_ERR_PATH_ESCAPE:   return -EACCES;
    case VFS_ERR_PATH_BADCHAR:  return -EINVAL;
    default:                    return -EIO;
    }
}

/* The parent directory used for membership/NOTIFY: "" -> "/". */
static const char *parent_dir(const vfs_canon_path *cp) {
    return cp->parent[0] ? cp->parent : "/";
}

/* ------------------------------------------------------------------ */

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

static void fill_obj_stat(struct stat *st, const vfs_object_info *o) {
    memset(st, 0, sizeof *st);
    int is_link = o->kind && !strcmp(o->kind, "symlink");
    st->st_mode  = (is_link ? S_IFLNK : S_IFREG) | (mode_t)(o->mode & 07777);
    st->st_nlink = 1;
    st->st_uid   = (o->uid >= 0) ? (uid_t)o->uid : getuid();
    st->st_gid   = (o->gid >= 0) ? (gid_t)o->gid : getgid();
    st->st_size  = o->size;
    st->st_blocks = (o->size + 511) / 512;
    st->st_ctim.tv_sec  = o->ctime_ns / 1000000000;
    st->st_ctim.tv_nsec = o->ctime_ns % 1000000000;
    st->st_mtim.tv_sec  = o->mtime_ns / 1000000000;
    st->st_mtim.tv_nsec = o->mtime_ns % 1000000000;
    st->st_atim.tv_sec  = o->atime_ns / 1000000000;
    st->st_atim.tv_nsec = o->atime_ns % 1000000000;
}

/* Match an object-id prefix among same-named members (D2 disambiguation). */
struct idprefix_match { const char *prefix; vfs_object_id id; int count; };
static void idprefix_cb(const vfs_object_info *o, void *ud) {
    struct idprefix_match *m = ud;
    size_t pl = strlen(m->prefix);
    if (strncmp(o->id.hex, m->prefix, pl) == 0) { m->id = o->id; m->count++; }
}

/* Resolve a member of `parent` named `name`, transparently handling the
 * "name~<idprefix>" form that readdir emits for same-named objects (D2).
 * Caller holds the lock. Returns 0 + *id, or -errno. */
static int resolve_member(const char *parent, const char *name,
                          vfs_object_id *id) {
    vfs_error e = vfs_dir_member_by_name(CTX.store, CTX.view_name, parent, name, id);
    if (e == VFS_OK) return 0;                       /* exact, unique */
    if (e != VFS_ERR_NOTFOUND && e != VFS_ERR_AMBIGUOUS) return -EIO;

    /* Try to parse a "base~hexprefix" disambiguated name. */
    const char *tilde = strrchr(name, '~');
    if (!tilde || tilde == name || !tilde[1])
        return (e == VFS_ERR_AMBIGUOUS) ? -EIO : -ENOENT;
    for (const char *p = tilde + 1; *p; p++)
        if (!isxdigit((unsigned char)*p))
            return (e == VFS_ERR_AMBIGUOUS) ? -EIO : -ENOENT;
    size_t blen = (size_t)(tilde - name);
    if (blen > VFS_NAME_MAX) return -ENOENT;
    char base[VFS_NAME_MAX + 1];
    memcpy(base, name, blen); base[blen] = '\0';

    struct idprefix_match m = { .prefix = tilde + 1, .count = 0 };
    if (vfs_dir_members_named(CTX.store, CTX.view_name, parent, base,
                              idprefix_cb, &m) != VFS_OK) return -EIO;
    if (m.count == 1) { *id = m.id; return 0; }
    return -ENOENT;
}

/* Resolve a non-root canonical path to a file/symlink object. Must hold the
 * lock. Returns 0 + *id, or -errno. (-EISDIR if it names a directory.) */
static int resolve_file(const vfs_canon_path *cp, vfs_object_id *id) {
    int isdir = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, cp->path, &isdir) != VFS_OK)
        return -EIO;
    if (isdir) return -EISDIR;
    return resolve_member(parent_dir(cp), cp->name, id);
}

/* Apply (add=1) or remove (add=0) a directory's effective property pairs
 * to/from an object. Must hold the lock. */
struct prop_apply { vfs_object_id id; int add; int failed; };
static void prop_apply_cb(const vfs_dirprop_row *p, void *ud) {
    struct prop_apply *a = ud;
    vfs_error e = a->add
        ? vfs_object_prop_add  (CTX.store, &a->id, p->key, p->value)
        : vfs_object_prop_unset(CTX.store, &a->id, p->key, p->value);
    if (e != VFS_OK && !(!a->add && e == VFS_ERR_NOTFOUND)) a->failed++;
}
static int apply_dir_props(const char *dir, const vfs_object_id *id, int add) {
    struct prop_apply a = { .id = *id, .add = add, .failed = 0 };
    if (vfs_dir_prop_list(CTX.store, CTX.view_name, dir, 1, prop_apply_cb, &a)
        != VFS_OK) return -EIO;
    return a.failed ? -EIO : 0;
}

/* Ensure a directory exists (mkdir -p), tolerating "already exists". Must
 * hold the lock. */
static int ensure_dir(const char *dir) {
    vfs_error e = vfs_dir_create(CTX.store, CTX.view_name, dir, 0755);
    if (e == VFS_OK || e == VFS_ERR_EXISTS) return 0;
    if (e == VFS_ERR_NOTFOUND) return -ENOENT;   /* view gone */
    return -EIO;
}

static void notify(const char *parent) {
    vfs_notify_path(CTX.store, CTX.view_name, parent ? parent : "");
}

/* ------------------------------------------------------------------
 * getattr / readdir
 * ------------------------------------------------------------------ */

static int op_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi) {
    (void)fi;
    trace("getattr %s", path);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;

    LOCK();
    vfs_dir_row d;
    if (vfs_dir_get(CTX.store, CTX.view_name, cp.path, &d) == VFS_OK) {
        fill_dir_stat(st, d.mode, d.mtime_ns);
        UNLOCK();
        return 0;
    }
    if (cp.is_root) { UNLOCK(); return -ENOENT; }
    vfs_object_id id;
    rc = resolve_file(&cp, &id);
    if (rc == -EISDIR) rc = 0;   /* shouldn't happen: dir_get already covered */
    if (rc) { UNLOCK(); return rc; }
    vfs_object_info info;
    if (vfs_object_get(CTX.store, &id, &info) != VFS_OK) { UNLOCK(); return -EIO; }
    fill_obj_stat(st, &info);
    UNLOCK();
    return 0;
}

/* readdir collects child-dir names and matching object names first, then
 * emits them — disambiguating same-named objects (and objects that clash
 * with a child-dir name) by appending "~<object-id-prefix>" (D2). */
struct dname { char name[VFS_NAME_MAX + 1]; };
struct dlist { struct dname *v; int n, cap, oom; };
struct ment  { char name[VFS_NAME_MAX + 1]; char id[VFS_OID_HEX_LEN + 1]; };
struct mlist { struct ment *v; int n, cap, oom; };

static void dlist_push(struct dlist *l, const char *name) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        void *p = realloc(l->v, (size_t)nc * sizeof *l->v);
        if (!p) { l->oom = 1; return; }
        l->v = p; l->cap = nc;
    }
    snprintf(l->v[l->n++].name, sizeof l->v[0].name, "%s", name);
}
static void mlist_push(struct mlist *l, const char *name, const char *id) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        void *p = realloc(l->v, (size_t)nc * sizeof *l->v);
        if (!p) { l->oom = 1; return; }
        l->v = p; l->cap = nc;
    }
    snprintf(l->v[l->n].name, sizeof l->v[0].name, "%s", name);
    snprintf(l->v[l->n].id,   sizeof l->v[0].id,   "%s", id);
    l->n++;
}
static void dcb(const vfs_dir_row *d, void *ud) { dlist_push(ud, d->name); }
static void mcb(const vfs_object_info *o, void *ud) {
    mlist_push(ud, o->name[0] ? o->name : o->id.hex, o->id.hex);
}
static int ment_cmp(const void *a, const void *b) {
    const struct ment *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return c ? c : strcmp(x->id, y->id);
}
static int common_prefix_len(const char *a, const char *b) {
    int i = 0; while (a[i] && a[i] == b[i]) i++; return i;
}
static int dlist_has(const struct dlist *l, const char *name) {
    for (int i = 0; i < l->n; i++) if (!strcmp(l->v[i].name, name)) return 1;
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

    struct dlist dl = {0};
    struct mlist ml = {0};
    LOCK();
    int isdir = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, cp.path, &isdir) != VFS_OK) {
        UNLOCK(); return -EIO;
    }
    if (!isdir) { UNLOCK(); return -ENOTDIR; }
    vfs_error e = vfs_dir_list_children(CTX.store, CTX.view_name, cp.path, dcb, &dl);
    if (e == VFS_OK)
        e = vfs_dir_members(CTX.store, CTX.view_name, cp.path, mcb, &ml);
    UNLOCK();
    if (e != VFS_OK || dl.oom || ml.oom) { free(dl.v); free(ml.v); return -EIO; }

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (int i = 0; i < dl.n; i++) filler(buf, dl.v[i].name, NULL, 0, 0);

    qsort(ml.v, (size_t)ml.n, sizeof *ml.v, ment_cmp);
    int i = 0;
    while (i < ml.n) {
        int j = i;
        while (j < ml.n && !strcmp(ml.v[j].name, ml.v[i].name)) j++;
        int gs = j - i;
        if (gs == 1 && !dlist_has(&dl, ml.v[i].name)) {
            filler(buf, ml.v[i].name, NULL, 0, 0);
        } else {
            /* collision: same name repeated, or clashes with a child dir.
             * Pick a uniquifying id-prefix length for the group. */
            int maxc = 0;
            for (int k = i + 1; k < j; k++) {
                int c = common_prefix_len(ml.v[k-1].id, ml.v[k].id);
                if (c > maxc) maxc = c;
            }
            int L = (gs == 1) ? 4 : maxc + 1;
            if (L < 4) L = 4;
            if (L > VFS_OID_HEX_LEN) L = VFS_OID_HEX_LEN;
            for (int k = i; k < j; k++) {
                char disp[VFS_NAME_MAX + 2 + VFS_OID_HEX_LEN];
                snprintf(disp, sizeof disp, "%s~%.*s", ml.v[k].name, L, ml.v[k].id);
                filler(buf, disp, NULL, 0, 0);
            }
        }
        i = j;
    }
    free(dl.v); free(ml.v);
    return 0;
}

/* ------------------------------------------------------------------
 * open / read / write / truncate / flush / fsync / release
 * ------------------------------------------------------------------ */

static int open_content_file(const vfs_object_id *id, int flags) {
    char p[VFS_PATH_MAX];
    if (vfs_content_path(CTX.store, id, p, sizeof p) != VFS_OK) return -EIO;
    int fd = open(p, flags | O_CLOEXEC);
    return (fd < 0) ? -errno : fd;
}

static struct vfs_ofile *ofile_new(int fd, int writable,
                                   const vfs_object_id *id, const char *parent) {
    struct vfs_ofile *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->fd = fd; o->writable = writable; o->id = *id;
    snprintf(o->parent, sizeof o->parent, "%s", parent);
    return o;
}

static int op_open(const char *path, struct fuse_file_info *fi) {
    trace("open %s flags=0x%x", path, fi->flags);
    int acc = fi->flags & O_ACCMODE;
    int wants_write = (acc != O_RDONLY);
    if (wants_write && CTX.read_only) return -EROFS;

    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;

    LOCK();
    vfs_object_id id;
    rc = resolve_file(&cp, &id);
    UNLOCK();
    if (rc) return rc;

    int host_flags = acc;
    if (fi->flags & O_TRUNC)  host_flags |= O_TRUNC;
    if (fi->flags & O_APPEND) host_flags |= O_APPEND;
    int fd = open_content_file(&id, host_flags);
    if (fd < 0) return fd;

    struct vfs_ofile *o = ofile_new(fd, wants_write, &id, parent_dir(&cp));
    if (!o) { close(fd); return -ENOMEM; }
    if (wants_write && (fi->flags & O_TRUNC)) o->modified = 1;
    fi->fh = (uint64_t)(uintptr_t)o;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    (void)path;
    int fd = VFS_OFILE(fi)->fd;
    ssize_t n;
    do { n = pread(fd, buf, size, off); } while (n < 0 && errno == EINTR);
    return (n < 0) ? -errno : (int)n;
}

static int op_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    (void)path;
    struct vfs_ofile *o = VFS_OFILE(fi);
    ssize_t n;
    do { n = pwrite(o->fd, buf, size, off); } while (n < 0 && errno == EINTR);
    if (n > 0) o->modified = 1;
    return (n < 0) ? -errno : (int)n;
}

/* Recompute the content checksum and persist size/mtime/atime + checksum
 * for object `id`. Caller holds the lock. */
static int persist_after_write(const vfs_object_id *id, int fd,
                               const char *parent) {
    struct stat stt;
    if (fstat(fd, &stt) != 0) return -errno;
    int64_t mt = (int64_t)stt.st_mtim.tv_sec * 1000000000LL + stt.st_mtim.tv_nsec;
    int64_t at = (int64_t)stt.st_atim.tv_sec * 1000000000LL + stt.st_atim.tv_nsec;
    if (vfs_object_set_stat(CTX.store, id, (int64_t)stt.st_size, mt, at) != VFS_OK)
        return -EIO;
    char cpath[VFS_PATH_MAX], hex[65];
    if (vfs_content_path(CTX.store, id, cpath, sizeof cpath) == VFS_OK &&
        vfs_sha256_hex_path(cpath, hex) == VFS_OK)
        vfs_object_set_checksum(CTX.store, id, hex, NULL, 0);
    else
        vfs_object_set_checksum(CTX.store, id, NULL, NULL, 0);
    notify(parent);
    return 0;
}

static int op_flush(const char *path, struct fuse_file_info *fi) {
    struct vfs_ofile *o = VFS_OFILE(fi);
    trace("flush %s", path);
    if (!o || !o->writable || !o->modified || o->fd <= 0) return 0;
    if (fsync(o->fd) != 0) return -errno;
    LOCK();
    int rc = persist_after_write(&o->id, o->fd, o->parent);
    UNLOCK();
    if (rc) return rc;
    o->modified = 0;
    return 0;
}

static int op_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path;
    struct vfs_ofile *o = VFS_OFILE(fi);
    if (!o || o->fd <= 0) return 0;
    int rc = datasync ? fdatasync(o->fd) : fsync(o->fd);
    return rc == 0 ? 0 : -errno;
}

static int op_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    struct vfs_ofile *o = VFS_OFILE(fi);
    if (o) { if (o->fd > 0) close(o->fd); free(o); }
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

    struct fuse_context *fctx = fuse_get_context();
    vfs_object_id id;

    LOCK();
    rc = ensure_dir(parent_dir(&cp));
    if (rc) { UNLOCK(); return rc; }
    vfs_error e = vfs_object_create_file(CTX.store, cp.name, (int)(mode & 07777),
                                         0, (int)fctx->uid, (int)fctx->gid,
                                         NULL, NULL, 0, NULL, 0, &id);
    if (e != VFS_OK) { UNLOCK(); return -EIO; }
    if (vfs_content_create_empty(CTX.store, &id) != VFS_OK) {
        vfs_object_delete(CTX.store, &id);
        UNLOCK();
        return -EIO;
    }
    rc = apply_dir_props(parent_dir(&cp), &id, 1);
    if (rc) { UNLOCK(); return rc; }
    notify(parent_dir(&cp));
    UNLOCK();

    int fd = open_content_file(&id, O_RDWR);
    if (fd < 0) return fd;
    struct vfs_ofile *o = ofile_new(fd, 1, &id, parent_dir(&cp));
    if (!o) { close(fd); return -ENOMEM; }
    fi->fh = (uint64_t)(uintptr_t)o;
    return 0;
}

static int op_truncate(const char *path, off_t off, struct fuse_file_info *fi) {
    trace("truncate %s off=%lld", path, (long long)off);
    if (CTX.read_only) return -EROFS;
    if (fi && fi->fh) {
        struct vfs_ofile *o = VFS_OFILE(fi);
        if (ftruncate(o->fd, off) != 0) return -errno;
        o->modified = 1;
        return 0;
    }
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    vfs_object_id id;
    LOCK();
    rc = resolve_file(&cp, &id);
    UNLOCK();
    if (rc) return rc;
    int fd = open_content_file(&id, O_RDWR);
    if (fd < 0) return fd;
    int rc2 = (ftruncate(fd, off) != 0) ? -errno : 0;
    if (rc2 == 0) {
        LOCK();
        persist_after_write(&id, fd, parent_dir(&cp));
        UNLOCK();
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

    LOCK();
    vfs_object_id id;
    rc = resolve_file(&cp, &id);             /* -EISDIR if it's a directory */
    if (rc) { UNLOCK(); return rc; }
    vfs_error e = vfs_object_delete(CTX.store, &id);
    if (e == VFS_OK) notify(parent_dir(&cp));
    UNLOCK();
    return e == VFS_OK ? 0 : -EIO;
}

static int op_mkdir(const char *path, mode_t mode) {
    trace("mkdir %s mode=%04o", path, mode);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EEXIST;
    LOCK();
    vfs_error e = vfs_dir_create(CTX.store, CTX.view_name, cp.path,
                                 (int)(mode & 07777));
    UNLOCK();
    switch (e) {
    case VFS_OK:           return 0;
    case VFS_ERR_EXISTS:   return -EEXIST;
    case VFS_ERR_NOTFOUND: return -ENOENT;
    default:               return -EIO;
    }
}

static int op_rmdir(const char *path) {
    trace("rmdir %s", path);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EBUSY;
    LOCK();
    vfs_error e = vfs_dir_remove(CTX.store, CTX.view_name, cp.path);
    UNLOCK();
    switch (e) {
    case VFS_OK:            return 0;
    case VFS_ERR_NOTFOUND:  return -ENOENT;
    case VFS_ERR_NOTEMPTY:  return -ENOTEMPTY;
    case VFS_ERR_BADARGS:   return -EBUSY;
    default:                return -EIO;
    }
}

/* ------------------------------------------------------------------
 * symlink / readlink
 * ------------------------------------------------------------------ */

static int validate_symlink_target(const char *target) {
    if (!target || !*target) return -EINVAL;
    if (strlen(target) >= VFS_PATH_MAX) return -ENAMETOOLONG;
    for (const unsigned char *p = (const unsigned char *)target; *p; p++)
        if (*p < 0x20) return -EINVAL;
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

    struct fuse_context *fctx = fuse_get_context();
    LOCK();
    rc = ensure_dir(parent_dir(&cp));
    if (rc) { UNLOCK(); return rc; }
    vfs_object_id id;
    vfs_error e = vfs_object_create_symlink(CTX.store, cp.name, target,
                                            (int)fctx->uid, (int)fctx->gid, &id);
    if (e != VFS_OK) { UNLOCK(); return -EIO; }
    rc = apply_dir_props(parent_dir(&cp), &id, 1);
    if (rc) { UNLOCK(); return rc; }
    notify(parent_dir(&cp));
    UNLOCK();
    return 0;
}

static int op_readlink(const char *path, char *buf, size_t size) {
    trace("readlink %s", path);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -EINVAL;
    if (size == 0) return -EINVAL;

    LOCK();
    vfs_object_id id;
    rc = resolve_file(&cp, &id);
    if (rc) { UNLOCK(); return rc; }
    char target[VFS_PATH_MAX];
    vfs_error e = vfs_object_symlink_target(CTX.store, &id, target, sizeof target);
    UNLOCK();
    if (e == VFS_ERR_NOTFOUND) return -EINVAL;   /* not a symlink */
    if (e != VFS_OK) return -EIO;
    size_t tlen = strlen(target);
    if (tlen >= size) tlen = size - 1;
    memcpy(buf, target, tlen);
    buf[tlen] = '\0';
    return 0;
}

/* ------------------------------------------------------------------
 * rename: file = move (edit object props); directory = subtree move
 * ------------------------------------------------------------------ */

static int op_rename(const char *from, const char *to, unsigned int flags) {
    trace("rename %s -> %s flags=0x%x", from, to, flags);
    if (CTX.read_only) return -EROFS;
    if (flags & RENAME_EXCHANGE) return -EINVAL;
    if (flags & ~((unsigned)RENAME_NOREPLACE)) return -EINVAL;

    vfs_canon_path sf, df;
    int rc = canon(from, &sf); if (rc) return rc;
    rc = canon(to, &df);       if (rc) return rc;
    if (sf.is_root || df.is_root) return -EBUSY;
    if (!strcmp(sf.path, df.path)) return 0;

    LOCK();
    int src_is_dir = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, sf.path, &src_is_dir) != VFS_OK) {
        UNLOCK(); return -EIO;
    }

    if (src_is_dir) {
        vfs_error e = vfs_dir_rename(CTX.store, CTX.view_name, sf.path, df.path);
        UNLOCK();
        switch (e) {
        case VFS_OK:           return 0;
        case VFS_ERR_NOTFOUND: return -ENOENT;
        case VFS_ERR_EXISTS:   return -EEXIST;
        case VFS_ERR_BADARGS:  return -EINVAL;
        default:               return -EIO;
        }
    }

    /* Source is a file/symlink. */
    vfs_object_id id;
    rc = resolve_file(&sf, &id);
    if (rc) { UNLOCK(); return rc; }

    /* Destination: reject if it is a directory; replace an existing file. */
    int dst_is_dir = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, df.path, &dst_is_dir) != VFS_OK) {
        UNLOCK(); return -EIO;
    }
    if (dst_is_dir) { UNLOCK(); return -EISDIR; }
    vfs_object_id dvictim;
    vfs_error de = vfs_dir_member_by_name(CTX.store, CTX.view_name,
                                          parent_dir(&df), df.name, &dvictim);
    if (de == VFS_OK) {
        if (flags & RENAME_NOREPLACE) { UNLOCK(); return -EEXIST; }
        vfs_object_delete(CTX.store, &dvictim);
    } else if (de != VFS_ERR_NOTFOUND && de != VFS_ERR_AMBIGUOUS) {
        UNLOCK(); return -EIO;
    }

    rc = ensure_dir(parent_dir(&df));
    if (rc) { UNLOCK(); return rc; }

    /* Move = lose source-dir pairs, gain dest-dir pairs, keep the rest. */
    int r1 = apply_dir_props(parent_dir(&sf), &id, 0);
    int r2 = apply_dir_props(parent_dir(&df), &id, 1);
    vfs_object_set_name(CTX.store, &id, df.name);
    notify(parent_dir(&sf));
    if (strcmp(parent_dir(&sf), parent_dir(&df)) != 0) notify(parent_dir(&df));
    UNLOCK();
    if (r1 || r2) return -EIO;
    return 0;
}

/* ------------------------------------------------------------------
 * utimens / chmod / chown
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

    int64_t now = vfs_now_ns();
    int64_t at = (ts[0].tv_nsec == UTIME_NOW || ts[0].tv_nsec == UTIME_OMIT)
        ? now : (int64_t)ts[0].tv_sec * 1000000000LL + ts[0].tv_nsec;
    int64_t mt = (ts[1].tv_nsec == UTIME_NOW || ts[1].tv_nsec == UTIME_OMIT)
        ? now : (int64_t)ts[1].tv_sec * 1000000000LL + ts[1].tv_nsec;

    LOCK();
    int isdir = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, cp.path, &isdir) != VFS_OK) {
        UNLOCK(); return -EIO;
    }
    if (isdir) { UNLOCK(); return 0; }   /* dir timestamps not tracked */
    vfs_object_id id;
    rc = resolve_file(&cp, &id);
    if (rc) { UNLOCK(); return rc; }
    vfs_error e = vfs_object_set_times(CTX.store, &id, mt, at);
    UNLOCK();
    return e == VFS_OK ? 0 : -EIO;
}

static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    trace("chmod %s mode=%04o", path, mode);
    if (CTX.read_only) return -EROFS;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return 0;

    LOCK();
    int isdir = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, cp.path, &isdir) != VFS_OK) {
        UNLOCK(); return -EIO;
    }
    vfs_error e;
    if (isdir) {
        e = vfs_dir_set_mode(CTX.store, CTX.view_name, cp.path, (int)(mode & 07777));
    } else {
        vfs_object_id id;
        rc = resolve_file(&cp, &id);
        if (rc) { UNLOCK(); return rc; }
        e = vfs_object_set_mode(CTX.store, &id, (int)(mode & 07777));
        notify(parent_dir(&cp));
    }
    UNLOCK();
    return e == VFS_OK ? 0 : -EIO;
}

static int op_chown(const char *path, uid_t uid, gid_t gid,
                    struct fuse_file_info *fi) {
    (void)fi;
    trace("chown %s uid=%u gid=%u", path, (unsigned)uid, (unsigned)gid);
    if (CTX.read_only) return -EROFS;
    int cu = (uid != (uid_t)-1), cg = (gid != (gid_t)-1);
    if (!cu && !cg) return 0;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return 0;

    LOCK();
    int isdir = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, cp.path, &isdir) != VFS_OK) {
        UNLOCK(); return -EIO;
    }
    if (isdir) { UNLOCK(); return 0; }   /* directories own no object */
    vfs_object_id id;
    rc = resolve_file(&cp, &id);
    if (rc) { UNLOCK(); return rc; }
    vfs_error e = vfs_object_set_owner(CTX.store, &id,
                                       cu ? (int)uid : -1, cg ? (int)gid : -1);
    UNLOCK();
    return e == VFS_OK ? 0 : -EIO;
}

/* ------------------------------------------------------------------
 * Extended attributes -> object_props under the "user.viewfs." prefix.
 * ------------------------------------------------------------------ */

static const char XATTR_PREFIX[] = "user.viewfs.";
#define XATTR_PREFIX_LEN (sizeof XATTR_PREFIX - 1)

/* Resolve a path to a file object for xattr ops. Holds-lock contract: caller
 * locks. Returns 0+id, or -errno (dirs -> -ENOTSUP). */
static int xattr_resolve(const vfs_canon_path *cp, vfs_object_id *id) {
    int isdir = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, cp->path, &isdir) != VFS_OK)
        return -EIO;
    if (isdir) return -ENOTSUP;          /* xattrs on directories deferred */
    return resolve_file(cp, id);
}

struct getx_ctx { const char *key; char *val; size_t len; int found; };
static void getx_cb(const vfs_prop_row *p, void *ud) {
    struct getx_ctx *g = ud;
    if (!g->found && !strcmp(p->key, g->key)) {
        g->len = strlen(p->value);
        g->val = malloc(g->len + 1);
        if (g->val) memcpy(g->val, p->value, g->len + 1);
        g->found = 1;
    }
}

static int op_getxattr(const char *path, const char *name, char *value, size_t size) {
    trace("getxattr %s name=%s", path, name);
    if (strncmp(name, XATTR_PREFIX, XATTR_PREFIX_LEN) != 0) return -ENOTSUP;
    const char *key = name + XATTR_PREFIX_LEN;
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return -ENODATA;

    LOCK();
    vfs_object_id id;
    rc = xattr_resolve(&cp, &id);
    if (rc) { UNLOCK(); return rc == -ENOTSUP ? -ENODATA : rc; }
    struct getx_ctx g = { .key = key, .val = NULL, .len = 0, .found = 0 };
    vfs_object_prop_list(CTX.store, &id, getx_cb, &g);
    UNLOCK();

    if (!g.found) return -ENODATA;
    int ret;
    if (size == 0)            ret = (int)g.len;
    else if (size < g.len)    ret = -ERANGE;
    else { memcpy(value, g.val, g.len); ret = (int)g.len; }
    free(g.val);
    return ret;
}

static int op_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags) {
    trace("setxattr %s name=%s", path, name);
    if (CTX.read_only) return -EROFS;
    if (strncmp(name, XATTR_PREFIX, XATTR_PREFIX_LEN) != 0) return -ENOTSUP;
    const char *key = name + XATTR_PREFIX_LEN;
    if (!*key) return -EINVAL;
    for (size_t i = 0; i < size; i++) if (value[i] == '\0') return -EINVAL;

    char *vcopy = malloc(size + 1);
    if (!vcopy) return -ENOMEM;
    memcpy(vcopy, value, size);
    vcopy[size] = '\0';

    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) { free(vcopy); return rc; }
    if (cp.is_root) { free(vcopy); return -EINVAL; }

    LOCK();
    vfs_object_id id;
    rc = xattr_resolve(&cp, &id);
    if (rc) { UNLOCK(); free(vcopy); return rc; }

    /* Does the key currently exist (any value)? */
    struct getx_ctx g = { .key = key, .val = NULL, .len = 0, .found = 0 };
    vfs_object_prop_list(CTX.store, &id, getx_cb, &g);
    free(g.val);
    int ret = 0;
    if ((flags & XATTR_CREATE) && g.found)        ret = -EEXIST;
    else if ((flags & XATTR_REPLACE) && !g.found) ret = -ENODATA;
    else {
        /* xattr is single-valued: replace every value of key with this one. */
        vfs_object_prop_unset(CTX.store, &id, key, NULL);
        if (vfs_object_prop_add(CTX.store, &id, key, vcopy) != VFS_OK) ret = -EIO;
        else notify(parent_dir(&cp));
    }
    UNLOCK();
    free(vcopy);
    return ret;
}

struct listx_ctx { char keys[256][VFS_NAME_MAX + 1]; int n; char last[VFS_NAME_MAX + 1]; };
static void listx_cb(const vfs_prop_row *p, void *ud) {
    struct listx_ctx *l = ud;
    if (l->n > 0 && !strcmp(l->last, p->key)) return;   /* dedupe (ordered) */
    if (l->n < 256) {
        snprintf(l->keys[l->n], sizeof l->keys[l->n], "%s", p->key);
        l->n++;
    }
    snprintf(l->last, sizeof l->last, "%s", p->key);
}

static int op_listxattr(const char *path, char *list, size_t size) {
    trace("listxattr %s", path);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) return 0;

    LOCK();
    vfs_object_id id;
    rc = xattr_resolve(&cp, &id);
    if (rc) { UNLOCK(); return rc == -ENOTSUP ? 0 : rc; }
    static struct listx_ctx l;          /* big; under lock so single-use ok */
    l.n = 0; l.last[0] = '\0';
    vfs_object_prop_list(CTX.store, &id, listx_cb, &l);
    UNLOCK();

    size_t total = 0;
    for (int i = 0; i < l.n; i++)
        total += XATTR_PREFIX_LEN + strlen(l.keys[i]) + 1;
    if (size == 0)    return (int)total;
    if (size < total) return -ERANGE;
    size_t off = 0;
    for (int i = 0; i < l.n; i++) {
        memcpy(list + off, XATTR_PREFIX, XATTR_PREFIX_LEN); off += XATTR_PREFIX_LEN;
        size_t kl = strlen(l.keys[i]);
        memcpy(list + off, l.keys[i], kl); off += kl;
        list[off++] = '\0';
    }
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

    LOCK();
    vfs_object_id id;
    rc = xattr_resolve(&cp, &id);
    if (rc) { UNLOCK(); return rc == -ENOTSUP ? -ENODATA : rc; }
    vfs_error e = vfs_object_prop_unset(CTX.store, &id, key, NULL);
    if (e == VFS_OK) notify(parent_dir(&cp));
    UNLOCK();
    return e == VFS_OK ? 0 : (e == VFS_ERR_NOTFOUND ? -ENODATA : -EIO);
}

/* ------------------------------------------------------------------
 * statfs / access / init / destroy
 * ------------------------------------------------------------------ */

static int op_statfs(const char *path, struct statvfs *st) {
    (void)path;
    if (statvfs(vfs_store_path(CTX.store), st) != 0) return -errno;
    return 0;
}

static int op_access(const char *path, int mask) {
    trace("access %s mask=0x%x", path, mask);
    vfs_canon_path cp;
    int rc = canon(path, &cp);
    if (rc) return rc;
    if (cp.is_root) { if (CTX.read_only && (mask & W_OK)) return -EROFS; return 0; }

    LOCK();
    int isdir = 0, ok = 0;
    if (vfs_dir_exists(CTX.store, CTX.view_name, cp.path, &isdir) == VFS_OK) {
        if (isdir) ok = 1;
        else {
            vfs_object_id id;
            ok = (resolve_file(&cp, &id) == 0);
        }
    }
    UNLOCK();
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
    cfg->attr_timeout     = 2.0;
    cfg->entry_timeout    = 2.0;
    cfg->negative_timeout = 1.0;
    CTX.fuse_handle = fuse_get_context()->fuse;

    char p[VFS_PATH_MAX];
    pid_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }

    if (notify_thread_start(&CTX) != 0) {
        fprintf(stderr, "vfs-fuse: notify thread failed; cache disabled\n");
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
