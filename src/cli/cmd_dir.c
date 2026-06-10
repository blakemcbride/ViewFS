/* `vfs dir` -- manage the per-view directory tree (mkdir / rmdir / ls).
 * A directory's contents are computed (objects whose properties are a
 * superset of the directory's effective filter). Directory *filters* are
 * managed with `vfs prop` (a directory is just another `prop` target).
 *
 * Inside a mounted view, VIEW/DIR may be omitted and are taken from the
 * current directory; a lone DIR is resolved relative to the cwd (see
 * cli_resolve_dir). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"Usage: vfs dir <subcommand> [args]\n"
"  Inside a mounted view, VIEW/DIR may be omitted (taken from your cwd);\n"
"  a lone DIR is resolved relative to the current directory.\n"
"  dir mkdir [VIEW] DIR\n"
"  dir rmdir [VIEW] DIR\n"
"  dir ls [[VIEW] DIR]              computed contents (dirs + files)\n"
"\n"
"  Directory property filters are managed with `vfs prop` — e.g.\n"
"  `vfs prop set VIEW:DIR KEY VALUE [--flow]`, `vfs prop list VIEW:DIR`.\n");
}
static int usage(void) { print_usage(stderr); return 2; }

static void print_child(const vfs_dir_row *d, void *ud) {
    (void)ud;
    printf("  %s/\n", d->name);
}
static void print_member(const vfs_object_info *o, void *ud) {
    (void)ud;
    printf("  %-32s %s  %10lld\n",
           o->name[0] ? o->name : "(unnamed)", o->id.hex, (long long)o->size);
}

static int err_dir(vfs_store *s, vfs_error e, const char *view,
                   const char *dir, const char *ctx) {
    if (e == VFS_ERR_NOTFOUND) {
        fprintf(stderr, "vfs: no directory %s:%s\n", view, dir);
        return 1;
    }
    if (e == VFS_ERR_EXISTS) {
        fprintf(stderr, "vfs: %s:%s already exists\n", view, dir);
        return 1;
    }
    if (e == VFS_ERR_NOTEMPTY) {
        fprintf(stderr, "vfs: %s:%s is not empty\n", view, dir);
        return 1;
    }
    return cli_perror(s, e, ctx);
}

int cmd_dir(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *sub = argv[2];
    if (cli_is_help_request(sub)) { print_usage(stdout); return 0; }

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    char vb[128], db[VFS_PATH_MAX];
    int rc;
    if (!strcmp(sub, "mkdir") || !strcmp(sub, "rmdir")) {
        /* a target is required: [VIEW] DIR (1 or 2 leading positionals). */
        int n = (argc == 5) ? 2 : (argc == 4) ? 1 : -1;
        if (n < 0) { vfs_store_close(s); return usage(); }
        const char *a0 = argv[3], *a1 = (n == 2) ? argv[4] : NULL;
        if (cli_resolve_dir(n, a0, a1, vb, sizeof vb, db, sizeof db)) {
            vfs_store_close(s); return 1;
        }
        if (!strcmp(sub, "mkdir")) {
            vfs_error e = vfs_dir_create(s, vb, db, 0755);
            rc = (e == VFS_OK) ? (printf("Created %s:%s\n", vb, db), 0)
                               : err_dir(s, e, vb, db, "dir mkdir");
        } else {
            vfs_error e = vfs_dir_remove(s, vb, db);
            rc = (e == VFS_OK) ? (printf("Removed %s:%s\n", vb, db), 0)
                               : err_dir(s, e, vb, db, "dir rmdir");
        }
    } else if (!strcmp(sub, "ls")) {
        int n = (argc == 5) ? 2 : (argc == 4) ? 1 : (argc == 3) ? 0 : -1;
        if (n < 0) { vfs_store_close(s); return usage(); }
        const char *a0 = (n >= 1) ? argv[3] : NULL, *a1 = (n == 2) ? argv[4] : NULL;
        if (cli_resolve_dir(n, a0, a1, vb, sizeof vb, db, sizeof db)) {
            vfs_store_close(s); return 1;
        }
        int exists = 0;
        vfs_error e = vfs_dir_exists(s, vb, db, &exists);
        if (e != VFS_OK)      { rc = cli_perror(s, e, "dir ls"); goto done; }
        if (!exists)          { rc = err_dir(s, VFS_ERR_NOTFOUND, vb, db, NULL); goto done; }
        printf("%s:%s\n", vb, db);
        e = vfs_dir_list_children(s, vb, db, print_child, NULL);
        if (e == VFS_OK) e = vfs_dir_members(s, vb, db, print_member, NULL);
        rc = (e == VFS_OK) ? 0 : cli_perror(s, e, "dir ls");
    } else {
        vfs_store_close(s);
        return usage();
    }
done:
    vfs_store_close(s);
    return rc;
}
