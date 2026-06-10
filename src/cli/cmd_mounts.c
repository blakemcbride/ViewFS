/* `vfs mounts` -- list the ViewFS views currently mounted and where.
 *
 * Reads /proc/mounts (each daemon mounts with device "viewfs:<view>"), so it
 * needs no store and reports every ViewFS mount on the system regardless of
 * which store backs it. */

#include <stdio.h>
#include <string.h>

#include "common.h"

struct mounts_ctx { int n; };

static void print_mount(const char *view, const char *mountpoint,
                        int read_only, void *ud) {
    struct mounts_ctx *c = ud;
    if (c->n == 0)
        printf("%-20s %-6s %s\n", "VIEW", "MODE", "MOUNTPOINT");
    printf("%-20s %-6s %s\n", view, read_only ? "ro" : "rw", mountpoint);
    c->n++;
}

int cmd_mounts(int argc, char **argv) {
    if (argc > 2 && cli_is_help_request(argv[2])) {
        printf("Usage: vfs mounts\n"
               "  List the ViewFS views currently mounted and their "
               "mountpoints.\n");
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: vfs mounts\n");
        return 2;
    }

    struct mounts_ctx c = { .n = 0 };
    int rc = cli_foreach_viewfs_mount(print_mount, &c);
    if (rc < 0) {
        fprintf(stderr, "vfs: cannot read /proc/mounts\n");
        return 1;
    }
    if (c.n == 0) {
        fprintf(stderr, "No ViewFS views are currently mounted.\n");
        return 0;
    }
    return 0;
}
