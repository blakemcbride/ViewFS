#include <dirent.h>
#include <errno.h>
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
    /* helpers for cross-checks */
    vfs_store *store;
    int        verbose;
    int        do_fix;
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

/* ------------------------------------------------------------------------- */

int cmd_check(int argc, char **argv) {
    const char *fix     = cli_take_flag(&argc, argv, "--fix",     1);
    const char *verbose = cli_take_flag(&argc, argv, "--verbose", 1);
    if (!verbose) verbose = cli_take_flag(&argc, argv, "-v",      1);
    if (argc != 2) {
        fprintf(stderr,
            "Usage: viewfs check [--fix] [--verbose]\n"
            "\n"
            "  Runs a three-phase consistency scan over the store.\n"
            "  --fix removes orphan content files only (never deletes objects).\n");
        return 2;
    }

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    struct findings f = {
        .store = s,
        .verbose = verbose != NULL,
        .do_fix  = fix     != NULL,
    };
    phase1_db     (&f);
    phase2_content(&f);
    phase3_schema (&f);

    int problems =
        (f.bad_object_refs > 0) +
        (f.bad_dir_invariant > 0) +
        (f.content_missing > 0) +
        (f.content_size_mismatch > 0) +
        (f.content_orphan_files > f.fixed_orphan_files) +
        (f.schema_drift != 0);

    fprintf(stdout, "\n");
    if (problems == 0)
        fprintf(stdout, "Store is consistent.\n");
    else
        fprintf(stdout, "Found %d issue class(es). See above.\n", problems);

    vfs_store_close(s);
    return problems == 0 ? 0 : 1;
}
