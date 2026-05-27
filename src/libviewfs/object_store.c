#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

vfs_error vfs_content_path_internal(const vfs_store *s, const vfs_object_id *id,
                                    char *buf, size_t bufsz) {
    if (!s || !id || !buf) return VFS_ERR_BADARGS;
    /* shard_depth=1 → first two hex chars; depth 0 → no shard subdir */
    int depth = s->shard_depth > 0 ? s->shard_depth : 0;
    if (depth == 0) {
        int n = snprintf(buf, bufsz, "%s/%s/%s",
                         s->store_path, VFS_OBJECTS_DIR, id->hex);
        return (n < 0 || (size_t)n >= bufsz) ? VFS_ERR_IO : VFS_OK;
    }
    int n = snprintf(buf, bufsz, "%s/%s/%.2s/%s",
                     s->store_path, VFS_OBJECTS_DIR, id->hex, id->hex);
    return (n < 0 || (size_t)n >= bufsz) ? VFS_ERR_IO : VFS_OK;
}

vfs_error vfs_content_path(const vfs_store *s, const vfs_object_id *id,
                           char *buf, size_t bufsz) {
    return vfs_content_path_internal(s, id, buf, bufsz);
}

static vfs_error ensure_shard_dir(const vfs_store *s, const vfs_object_id *id) {
    if (s->shard_depth <= 0) return VFS_OK;
    char dir[VFS_PATH_MAX];
    int n = snprintf(dir, sizeof dir, "%s/%s/%.2s",
                     s->store_path, VFS_OBJECTS_DIR, id->hex);
    if (n < 0 || (size_t)n >= sizeof dir) return VFS_ERR_IO;
    if (mkdir(dir, 0755) == 0) return VFS_OK;
    if (errno == EEXIST) return VFS_OK;
    return VFS_ERR_IO;
}

/* Open the parent directory of `path` and fsync it. This is the standard
 * Linux idiom for making a rename or file creation durable: the file
 * data may be synced via fsync(fd), but the directory entry that names
 * the file can still be lost without an fsync of the directory itself. */
static int fsync_parent_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    char dir[VFS_PATH_MAX];
    if (!slash || slash == path) {
        dir[0] = '/'; dir[1] = '\0';
    } else {
        size_t dl = (size_t)(slash - path);
        if (dl >= sizeof dir) { errno = ENAMETOOLONG; return -1; }
        memcpy(dir, path, dl);
        dir[dl] = '\0';
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return -1;
    int rc = fsync(dfd);
    int saved = errno;
    close(dfd);
    if (rc != 0) { errno = saved; return -1; }
    return 0;
}

static vfs_error tmp_path(const vfs_store *s, char *buf, size_t bufsz) {
    /* mkstemp-like name under tmp/. Caller will rename to final location. */
    int n = snprintf(buf, bufsz, "%s/%s/import.XXXXXX",
                     s->store_path, VFS_TMP_DIR);
    return (n < 0 || (size_t)n >= bufsz) ? VFS_ERR_IO : VFS_OK;
}

vfs_error vfs_content_import_host(vfs_store *s, const char *host_path,
                                  const vfs_object_id *id,
                                  int64_t *out_size,
                                  int     *out_mode,
                                  int64_t *out_mtime_ns,
                                  char    *checksum_hex_out,
                                  void    *checksum_state_out,
                                  size_t  *checksum_state_len_out) {
    if (!s || !host_path || !id) return VFS_ERR_BADARGS;

    struct stat st;
    if (stat(host_path, &st) != 0) {
        vfs_seterr(s, "stat %s: %s", host_path, strerror(errno));
        return VFS_ERR_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        vfs_seterr(s, "%s: not a regular file", host_path);
        return VFS_ERR_BADARGS;
    }

    char tmp[VFS_PATH_MAX];
    vfs_error rc = tmp_path(s, tmp, sizeof tmp);
    if (rc != VFS_OK) return rc;
    int tfd = mkstemp(tmp);
    if (tfd < 0) {
        vfs_seterr(s, "mkstemp %s: %s", tmp, strerror(errno));
        return VFS_ERR_IO;
    }
    int sfd = open(host_path, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) {
        vfs_seterr(s, "open %s: %s", host_path, strerror(errno));
        close(tfd);
        unlink(tmp);
        return VFS_ERR_IO;
    }

    /* If the caller asked for the hash and/or its intermediate state,
     * stream-hash the bytes as they pass through this copy loop. */
    vfs_sha256_stream sha = { 0 };
    int want_hash = (checksum_hex_out != NULL) || (checksum_state_out != NULL);
    if (want_hash) {
        rc = vfs_sha256_stream_init(&sha);
        if (rc != VFS_OK) {
            close(sfd); close(tfd); unlink(tmp);
            return rc;
        }
    }

    char buf[64 * 1024];
    for (;;) {
        ssize_t r = read(sfd, buf, sizeof buf);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            vfs_seterr(s, "read %s: %s", host_path, strerror(errno));
            if (want_hash) vfs_sha256_stream_abort(&sha);
            close(sfd); close(tfd); unlink(tmp);
            return VFS_ERR_IO;
        }
        if (want_hash) vfs_sha256_stream_update(&sha, buf, (size_t)r);
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(tfd, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                vfs_seterr(s, "write %s: %s", tmp, strerror(errno));
                if (want_hash) vfs_sha256_stream_abort(&sha);
                close(sfd); close(tfd); unlink(tmp);
                return VFS_ERR_IO;
            }
            off += w;
        }
    }
    close(sfd);

    /* Snapshot the state (so the FUSE daemon can resume appending later)
     * BEFORE finalizing, since finalize consumes the context. */
    if (want_hash) {
        if (checksum_state_out) {
            vfs_sha256_stream_snapshot(&sha, checksum_state_out);
            if (checksum_state_len_out)
                *checksum_state_len_out = VFS_SHA256_STATE_LEN;
        }
        if (checksum_hex_out) {
            vfs_sha256_stream_finalize(&sha, checksum_hex_out);
        } else {
            vfs_sha256_stream_abort(&sha);
        }
    }
    if (fsync(tfd) != 0) {
        vfs_seterr(s, "fsync %s: %s", tmp, strerror(errno));
        close(tfd); unlink(tmp);
        return VFS_ERR_IO;
    }
    if (close(tfd) != 0) {
        vfs_seterr(s, "close %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return VFS_ERR_IO;
    }

    if ((rc = ensure_shard_dir(s, id)) != VFS_OK) {
        unlink(tmp);
        return rc;
    }
    char final[VFS_PATH_MAX];
    if ((rc = vfs_content_path_internal(s, id, final, sizeof final)) != VFS_OK) {
        unlink(tmp);
        return rc;
    }
    if (rename(tmp, final) != 0) {
        vfs_seterr(s, "rename %s -> %s: %s", tmp, final, strerror(errno));
        unlink(tmp);
        return VFS_ERR_IO;
    }
    /* The rename is atomic in POSIX, but the directory entry naming the
     * file isn't durable until we fsync the parent. */
    if (fsync_parent_dir(final) != 0) {
        vfs_seterr(s, "fsync parent of %s: %s", final, strerror(errno));
        return VFS_ERR_IO;
    }
    /* Preserve mode bits + mtime. */
    if (chmod(final, st.st_mode & 07777) != 0) {
        /* not fatal */
    }
    struct timespec times[2] = { st.st_atim, st.st_mtim };
    utimensat(AT_FDCWD, final, times, 0);

    if (out_size)    *out_size    = (int64_t)st.st_size;
    if (out_mode)    *out_mode    = (int)(st.st_mode & 07777);
    if (out_mtime_ns) {
        *out_mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL
                        + st.st_mtim.tv_nsec;
    }
    return VFS_OK;
}

vfs_error vfs_content_create_empty(vfs_store *s, const vfs_object_id *id) {
    if (!s || !id) return VFS_ERR_BADARGS;
    vfs_error rc = ensure_shard_dir(s, id);
    if (rc != VFS_OK) return rc;
    char final[VFS_PATH_MAX];
    if ((rc = vfs_content_path_internal(s, id, final, sizeof final)) != VFS_OK)
        return rc;
    int fd = open(final, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return VFS_ERR_EXISTS;
        vfs_seterr(s, "create %s: %s", final, strerror(errno));
        return VFS_ERR_IO;
    }
    if (fsync(fd) != 0) {
        vfs_seterr(s, "fsync %s: %s", final, strerror(errno));
        close(fd); unlink(final);
        return VFS_ERR_IO;
    }
    close(fd);
    /* The directory entry naming this brand-new file is durable only
     * once we fsync the parent directory. */
    if (fsync_parent_dir(final) != 0) {
        vfs_seterr(s, "fsync parent of %s: %s", final, strerror(errno));
        unlink(final);
        return VFS_ERR_IO;
    }
    return VFS_OK;
}

vfs_error vfs_content_unlink(vfs_store *s, const vfs_object_id *id) {
    if (!s || !id) return VFS_ERR_BADARGS;
    char final[VFS_PATH_MAX];
    vfs_error rc = vfs_content_path_internal(s, id, final, sizeof final);
    if (rc != VFS_OK) return rc;
    if (unlink(final) != 0 && errno != ENOENT) {
        vfs_seterr(s, "unlink %s: %s", final, strerror(errno));
        return VFS_ERR_IO;
    }
    return VFS_OK;
}
