#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"

/* Findings accumulator. Each field counts a distinct issue class. */
struct findings {
    int64_t orphan_objects;          /* objects with no mappings */
    int64_t bad_object_refs;         /* mappings.object_id -> missing object */
    int64_t bad_dir_invariant;       /* mappings violating the CHECK */
    int64_t content_missing;         /* objects with no content file */
    int64_t content_size_mismatch;   /* objects whose content size != DB size */
    int64_t content_orphan_files;    /* content files with no object row */
    int     schema_drift;            /* 0 OK, 1 ahead, -1 behind */
    int     fixed_orphan_files;      /* --fix removals */
    /* Phase 4: checksum coverage + verification */
    int64_t files_total;             /* objects with kind='file' */
    int64_t files_with_checksum;     /* checksum column non-NULL */
    int64_t checksums_filled;        /* --fill-checksums: newly populated */
    int64_t checksums_fill_failed;   /* --fill-checksums: failed to hash */
    int64_t checksums_verified_ok;   /* --verify-checksums: matched */
    int64_t checksums_mismatch;      /* --verify-checksums: stored != recomputed */
    int64_t checksums_verify_failed; /* --verify-checksums: couldn't read content */
    /* helpers for cross-checks */
    vfs_store *store;
    int        verbose;
    int        do_fix;
    int        do_fill;
    int        do_verify;
};

/* Phase 1: DB integrity ---------------------------------------------------- */

static void inc_orphan(const vfs_object_info *o, void *ud) {
    (void)o;
    ((struct findings*)ud)->orphan_objects++;
}

static int phase1_db(struct findings *f) {
    fprintf(stdout, "[1/3] DB integrity:\n");
    vfs_object_list_orphans(f->store, inc_orphan, f);
    vfs_check_mappings_bad_objref    (f->store, &f->bad_object_refs);
    vfs_check_mappings_dir_invariant (f->store, &f->bad_dir_invariant);

    fprintf(stdout, "  orphan objects (no mappings):       %lld\n",
            (long long)f->orphan_objects);
    fprintf(stdout, "  mappings with dangling object_id:   %lld\n",
            (long long)f->bad_object_refs);
    fprintf(stdout, "  mappings violating dir-invariant:   %lld\n",
            (long long)f->bad_dir_invariant);
    return 0;
}

/* Phase 2: content vs object cross-check ---------------------------------- */

static void check_one_object(const vfs_object_info *o, void *ud) {
    struct findings *f = ud;

    /* symlinks have no content file -- only their target string lives in
     * the DB. Skip them in this cross-check. */
    if (o->kind && !strcmp(o->kind, "symlink")) return;

    char path[VFS_PATH_MAX];
    if (vfs_content_path(f->store, &o->id, path, sizeof path) != VFS_OK) {
        f->content_missing++;
        return;
    }
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (f->verbose)
            fprintf(stdout, "    MISSING: %s (id=%s)\n", path, o->id.hex);
        f->content_missing++;
        return;
    }
    if ((int64_t)st.st_size != o->size) {
        if (f->verbose)
            fprintf(stdout,
                "    SIZE-MISMATCH: id=%s db=%lld disk=%lld\n",
                o->id.hex, (long long)o->size, (long long)st.st_size);
        f->content_size_mismatch++;
    }
}

/* Walk objects/ on disk; for each content file, check that an objects
 * row exists. Counts orphan-on-disk files and (with --fix) deletes them. */
static void cross_check_disk(struct findings *f) {
    char obj_dir[VFS_PATH_MAX];
    snprintf(obj_dir, sizeof obj_dir, "%s/objects", vfs_store_path(f->store));
    DIR *shard_dir = opendir(obj_dir);
    if (!shard_dir) return;
    struct dirent *shard_e;
    while ((shard_e = readdir(shard_dir)) != NULL) {
        if (shard_e->d_name[0] == '.') continue;
        char inner[VFS_PATH_MAX];
        int n = snprintf(inner, sizeof inner, "%s/%s",
                         obj_dir, shard_e->d_name);
        if (n < 0 || (size_t)n >= sizeof inner) continue;

        DIR *d = opendir(inner);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (!vfs_object_id_valid(e->d_name)) {
                if (f->verbose)
                    fprintf(stdout, "    NOT-AN-ID: %s/%s (ignored)\n",
                            inner, e->d_name);
                continue;
            }
            vfs_object_id id;
            memcpy(id.hex, e->d_name, sizeof id.hex);
            id.hex[VFS_OID_HEX_LEN] = '\0';

            vfs_object_info info;
            vfs_error rc = vfs_object_get(f->store, &id, &info);
            if (rc == VFS_ERR_NOTFOUND) {
                f->content_orphan_files++;
                char fpath[VFS_PATH_MAX];
                int fn = snprintf(fpath, sizeof fpath, "%s/%s",
                                  inner, e->d_name);
                if (fn < 0 || (size_t)fn >= sizeof fpath) continue;
                if (f->do_fix) {
                    if (unlink(fpath) == 0) {
                        f->fixed_orphan_files++;
                        fprintf(stdout, "    FIXED: removed orphan file %s\n",
                                fpath);
                    } else {
                        fprintf(stderr,
                            "    unlink %s failed: %s\n",
                            fpath, strerror(errno));
                    }
                } else if (f->verbose) {
                    fprintf(stdout, "    ORPHAN-FILE: %s\n", fpath);
                }
            }
        }
        closedir(d);
    }
    closedir(shard_dir);
}

static int phase2_content(struct findings *f) {
    fprintf(stdout, "[2/3] Content <-> object cross-check:\n");
    vfs_object_list(f->store, check_one_object, f);
    cross_check_disk(f);

    fprintf(stdout, "  objects with missing content file:  %lld\n",
            (long long)f->content_missing);
    fprintf(stdout, "  objects with size mismatch:         %lld\n",
            (long long)f->content_size_mismatch);
    fprintf(stdout, "  content files with no object row:   %lld%s\n",
            (long long)f->content_orphan_files,
            f->do_fix ? " (removed)" : "");
    return 0;
}

/* Phase 3: schema version ------------------------------------------------- */

static int phase3_schema(struct findings *f) {
    fprintf(stdout, "[3/3] Schema version:\n");
    int db_ver = -1;
    vfs_error rc = vfs_schema_version(f->store, &db_ver);
    if (rc != VFS_OK) {
        fprintf(stdout, "  could not read schema_migrations: %s\n",
                vfs_store_last_error(f->store));
        return 1;
    }
    int want = VIEWFS_SCHEMA_VERSION;
    fprintf(stdout, "  DB schema version:   %d\n", db_ver);
    fprintf(stdout, "  Binary expects:      %d\n", want);
    if (db_ver == want) {
        fprintf(stdout, "  OK\n");
        f->schema_drift = 0;
    } else if (db_ver > want) {
        fprintf(stdout, "  WARN: DB is newer; this binary may misinterpret newer columns\n");
        f->schema_drift = 1;
    } else {
        fprintf(stdout, "  ERROR: DB is older than binary expects; re-run `viewfs init --reinit` or apply migrations\n");
        f->schema_drift = -1;
    }
    return 0;
}

/* Phase 4: checksum coverage (always) + optional fill / verify. */

static void checksum_phase_cb(const vfs_object_info *o, void *ud) {
    struct findings *f = ud;
    if (!o->kind || strcmp(o->kind, "file") != 0) return;
    f->files_total++;

    if (o->checksum) {
        f->files_with_checksum++;
        if (f->do_verify) {
            char path[VFS_PATH_MAX];
            if (vfs_content_path(f->store, &o->id, path, sizeof path) != VFS_OK) {
                f->checksums_verify_failed++;
                return;
            }
            char recomputed[65];
            if (vfs_sha256_hex_path(path, recomputed) != VFS_OK) {
                f->checksums_verify_failed++;
                if (f->verbose)
                    fprintf(stdout, "    VERIFY-IO-ERROR: id=%s\n", o->id.hex);
                return;
            }
            if (strcmp(recomputed, o->checksum) == 0) {
                f->checksums_verified_ok++;
            } else {
                f->checksums_mismatch++;
                if (f->verbose)
                    fprintf(stdout,
                        "    CHECKSUM-MISMATCH: id=%s stored=%s recomputed=%s\n",
                        o->id.hex, o->checksum, recomputed);
            }
        }
    } else if (f->do_fill) {
        /* Recompute hash AND state so subsequent appends can resume. */
        char path[VFS_PATH_MAX];
        if (vfs_content_path(f->store, &o->id, path, sizeof path) != VFS_OK) {
            f->checksums_fill_failed++;
            return;
        }
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            f->checksums_fill_failed++;
            return;
        }
        vfs_sha256_stream sha = { 0 };
        if (vfs_sha256_stream_init(&sha) != VFS_OK) {
            close(fd); f->checksums_fill_failed++; return;
        }
        unsigned char buf[64 * 1024];
        int ok = 1;
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = 0; break;
            }
            if (vfs_sha256_stream_update(&sha, buf, (size_t)n) != VFS_OK) {
                ok = 0; break;
            }
        }
        close(fd);
        if (!ok) {
            vfs_sha256_stream_abort(&sha);
            f->checksums_fill_failed++;
            return;
        }
        unsigned char state[VFS_SHA256_STATE_LEN];
        char hex[65];
        if (vfs_sha256_stream_snapshot(&sha, state) != VFS_OK ||
            vfs_sha256_stream_finalize(&sha, hex)   != VFS_OK) {
            f->checksums_fill_failed++;
            return;
        }
        if (vfs_object_set_checksum(f->store, &o->id, hex, state, sizeof state)
            != VFS_OK) {
            f->checksums_fill_failed++;
            return;
        }
        f->checksums_filled++;
        f->files_with_checksum++;
        if (f->verbose)
            fprintf(stdout, "    FILLED: id=%s checksum=%s\n", o->id.hex, hex);
    }
}

static int phase4_checksums(struct findings *f) {
    fprintf(stdout, "[4/4] Checksum coverage:\n");
    vfs_object_list(f->store, checksum_phase_cb, f);
    fprintf(stdout, "  file objects:                       %lld\n",
            (long long)f->files_total);
    fprintf(stdout, "  with checksum:                      %lld\n",
            (long long)f->files_with_checksum);
    fprintf(stdout, "  without checksum:                   %lld\n",
            (long long)(f->files_total - f->files_with_checksum));
    if (f->do_fill) {
        fprintf(stdout, "  filled (this run):                  %lld\n",
                (long long)f->checksums_filled);
        if (f->checksums_fill_failed)
            fprintf(stdout, "  fill failures:                      %lld\n",
                    (long long)f->checksums_fill_failed);
    }
    if (f->do_verify) {
        fprintf(stdout, "  verified ok:                        %lld\n",
                (long long)f->checksums_verified_ok);
        fprintf(stdout, "  mismatches:                         %lld\n",
                (long long)f->checksums_mismatch);
        if (f->checksums_verify_failed)
            fprintf(stdout, "  verify failures (I/O):              %lld\n",
                    (long long)f->checksums_verify_failed);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */

int cmd_check(int argc, char **argv) {
    const char *fix       = cli_take_flag(&argc, argv, "--fix",              1);
    const char *fill_cs   = cli_take_flag(&argc, argv, "--fill-checksums",   1);
    const char *verify_cs = cli_take_flag(&argc, argv, "--verify-checksums", 1);
    const char *verbose   = cli_take_flag(&argc, argv, "--verbose",          1);
    if (!verbose) verbose = cli_take_flag(&argc, argv, "-v",                 1);
    if (argc != 2) {
        fprintf(stderr,
            "Usage: viewfs check [--fix] [--fill-checksums] "
            "[--verify-checksums] [--verbose]\n"
            "\n"
            "  Four-phase consistency scan + checksum-coverage report.\n"
            "  --fix               removes orphan content files only.\n"
            "  --fill-checksums    compute SHA-256 (+ resumable state) for\n"
            "                      every file object with a NULL checksum.\n"
            "  --verify-checksums  re-hash every file with a stored checksum\n"
            "                      and report mismatches.\n");
        return 2;
    }

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    struct findings f = {
        .store     = s,
        .verbose   = verbose   != NULL,
        .do_fix    = fix       != NULL,
        .do_fill   = fill_cs   != NULL,
        .do_verify = verify_cs != NULL,
    };
    phase1_db       (&f);
    phase2_content  (&f);
    phase3_schema   (&f);
    phase4_checksums(&f);

    int problems =
        (f.bad_object_refs > 0) +
        (f.bad_dir_invariant > 0) +
        (f.content_missing > 0) +
        (f.content_size_mismatch > 0) +
        (f.content_orphan_files > f.fixed_orphan_files) +
        (f.schema_drift != 0) +
        (f.checksums_mismatch > 0);

    fprintf(stdout, "\n");
    if (problems == 0)
        fprintf(stdout, "Store is consistent.\n");
    else
        fprintf(stdout, "Found %d issue class(es). See above.\n", problems);

    vfs_store_close(s);
    return problems == 0 ? 0 : 1;
}
