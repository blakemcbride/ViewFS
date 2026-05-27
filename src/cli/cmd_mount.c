#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

static int usage(void) {
    fprintf(stderr,
"Usage: viewfs mount VIEW [--ro] [--foreground] [--verbose] MOUNTPOINT\n"
"\n"
"  Delegates to viewfs-fuse. The daemon backgrounds itself unless\n"
"  --foreground is given.\n");
    return 2;
}

/* Find the viewfs-fuse binary next to the running viewfs binary, else
 * rely on PATH lookup. Returns a malloc'd path or NULL (meaning use
 * execvp("viewfs-fuse", ...)). */
static char *locate_daemon(void) {
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return NULL;
    self[n] = '\0';
    char *dir = dirname(self);
    char path[4096];
    snprintf(path, sizeof path, "%s/viewfs-fuse", dir);
    if (access(path, X_OK) == 0) return strdup(path);
    return NULL;
}

int cmd_mount(int argc, char **argv) {
    const char *ro         = cli_take_flag(&argc, argv, "--ro", 1);
    const char *foreground = cli_take_flag(&argc, argv, "--foreground", 1);
    if (!foreground) foreground = cli_take_flag(&argc, argv, "-f", 1);
    const char *verbose    = cli_take_flag(&argc, argv, "--verbose", 1);
    if (!verbose) verbose = cli_take_flag(&argc, argv, "-v", 1);

    /* After flag consumption argv should be: viewfs, mount, VIEW, MOUNTPOINT */
    if (argc != 4) return usage();
    const char *view       = argv[2];
    const char *mountpoint = argv[3];

    const char *store = getenv("VIEWFS_STORE");
    if (!store || !*store) {
        fprintf(stderr,
            "viewfs: store path required (pass --store PATH or set VIEWFS_STORE)\n");
        return 2;
    }

    char *daemon_path = locate_daemon();
    const char *prog = daemon_path ? daemon_path : "viewfs-fuse";

    char *cargv[16];
    int  c = 0;
    cargv[c++] = (char*)prog;
    cargv[c++] = (char*)"--store";  cargv[c++] = (char*)store;
    cargv[c++] = (char*)"--view";   cargv[c++] = (char*)view;
    if (ro)         cargv[c++] = (char*)"--ro";
    if (foreground) cargv[c++] = (char*)"--foreground";
    if (verbose)    cargv[c++] = (char*)"--verbose";
    cargv[c++] = (char*)mountpoint;
    cargv[c]   = NULL;

    execvp(prog, cargv);
    fprintf(stderr, "viewfs: failed to exec %s: %s\n", prog, strerror(errno));
    free(daemon_path);
    return 127;
}
