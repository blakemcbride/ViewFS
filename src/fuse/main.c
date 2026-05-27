#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ops.h"
#include "conn_pool.h"

static void print_usage(FILE *out) {
    fprintf(out,
"viewfs-fuse %s -- view-based filesystem FUSE daemon\n"
"\n"
"Usage: viewfs-fuse --store STORE_PATH --view VIEW_NAME MOUNTPOINT [opts]\n"
"\n"
"Options:\n"
"  --store PATH        backing store directory (required)\n"
"  --view  NAME        view to expose at the mountpoint (required)\n"
"  --foreground, -f    do not daemonize (stay attached to the terminal)\n"
"  --ro                read-only mount\n"
"  --verbose, -v       enable verbose per-op tracing on stderr\n"
"  --force-empty       allow mounting over a non-empty directory\n"
"  --help, -h          show this message\n"
"  --version, -V       print the version\n",
        viewfs_version_string());
}

/* Extract --flag VALUE / --flag=VALUE from argv (mutates). Returns the
 * value or NULL. boolean=1 treats it as a switch. */
static const char *take_flag(int *argc, char **argv, const char *flag, int boolean) {
    size_t flen = strlen(flag);
    for (int i = 1; i < *argc; i++) {
        if (!strcmp(argv[i], flag)) {
            if (boolean) {
                for (int j = i; j < *argc - 1; j++) argv[j] = argv[j+1];
                (*argc)--;
                return "1";
            }
            if (i + 1 >= *argc) return NULL;
            const char *v = argv[i+1];
            for (int j = i; j < *argc - 2; j++) argv[j] = argv[j+2];
            (*argc) -= 2;
            return v;
        }
        if (!boolean && !strncmp(argv[i], flag, flen) && argv[i][flen] == '=') {
            const char *v = argv[i] + flen + 1;
            for (int j = i; j < *argc - 1; j++) argv[j] = argv[j+1];
            (*argc)--;
            return v;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    /* --help / --version are checked first, before any setup. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            printf("viewfs-fuse %s\n", viewfs_version_string());
            return 0;
        }
    }

    const char *store_path  = take_flag(&argc, argv, "--store", 0);
    const char *view_name   = take_flag(&argc, argv, "--view",  0);
    const char *fg          = take_flag(&argc, argv, "--foreground", 1);
    if (!fg) fg = take_flag(&argc, argv, "-f", 1);
    const char *ro          = take_flag(&argc, argv, "--ro", 1);
    const char *verbose     = take_flag(&argc, argv, "--verbose", 1);
    if (!verbose) verbose   = take_flag(&argc, argv, "-v", 1);
    const char *force_empty = take_flag(&argc, argv, "--force-empty", 1);
    (void)force_empty;

    if (!store_path || !view_name || argc != 2) {
        print_usage(stderr);
        return 2;
    }
    const char *mountpoint = argv[1];

    /* Open the store: this is also where libpq settings are validated. */
    vfs_store *store = NULL;
    vfs_error rc = vfs_store_open(store_path, &store);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs-fuse: cannot open store %s\n", store_path);
        return 1;
    }

    /* Confirm the view exists before we hand off to fuse_main. */
    int exists = 0;
    rc = vfs_view_exists(store, view_name, &exists);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs-fuse: view lookup failed: %s\n",
                vfs_store_last_error(store));
        vfs_store_close(store);
        return 1;
    }
    if (!exists) {
        fprintf(stderr, "viewfs-fuse: view '%s' does not exist in %s\n",
                view_name, store_path);
        vfs_store_close(store);
        return 1;
    }

    /* Build the FUSE daemon-wide context. */
    CTX.store     = store;
    CTX.read_only = (ro != NULL);
    CTX.verbose   = (verbose != NULL);
    snprintf(CTX.view_name, sizeof CTX.view_name, "%s", view_name);

    static conn_pool POOL;
    rc = conn_pool_init(&POOL, vfs_store_conninfo(store), vfs_store_schema(store));
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs-fuse: conn_pool init failed\n");
        vfs_store_close(store);
        return 1;
    }
    CTX.pool = &POOL;

    /* Translate our flags into a fuse_main argv:
     *
     *   [progname, "-o", "default_permissions",
     *              "-o", "fsname=viewfs:<view>",
     *              ("-f")?,
     *              ("-o","ro")?,
     *              mountpoint]
     */
    char fsname_opt[256];
    snprintf(fsname_opt, sizeof fsname_opt, "fsname=viewfs:%s", view_name);

    char *fuse_argv[16];
    int fa = 0;
    fuse_argv[fa++] = (char*)"viewfs-fuse";
    fuse_argv[fa++] = (char*)"-o"; fuse_argv[fa++] = (char*)"default_permissions";
    fuse_argv[fa++] = (char*)"-o"; fuse_argv[fa++] = fsname_opt;
    if (CTX.read_only) {
        fuse_argv[fa++] = (char*)"-o"; fuse_argv[fa++] = (char*)"ro";
    }
    if (fg) fuse_argv[fa++] = (char*)"-f";
    fuse_argv[fa++] = (char*)mountpoint;
    fuse_argv[fa]   = NULL;

    int ret = fuse_main(fa, fuse_argv, &viewfs_oper, NULL);

    conn_pool_destroy(&POOL);
    vfs_store_close(store);
    return ret;
}
