#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"viewfs %s -- view-based filesystem admin tool\n"
"\n"
"Usage: viewfs <command> [options...]\n"
"\n"
"Repository:\n"
"  init [STORE_PATH] [--pg CONNINFO] [--schema NAME] [--reinit]\n"
"  status                           show store status\n"
"\n"
"Views:\n"
"  view create NAME [--description TEXT]\n"
"  view list\n"
"  view show NAME\n"
"  view delete NAME\n"
"  view add VIEW OBJECT_ID|PREFIX VIEW_PATH\n"
"  view remove VIEW VIEW_PATH\n"
"  view populate VIEW --tag TAG --under VIEW_PATH\n"
"\n"
"Objects:\n"
"  object import HOST_PATH [--into VIEW:PATH]...\n"
"  object show ID|PREFIX\n"
"  object paths ID|PREFIX\n"
"  object id VIEW VIEW_PATH\n"
"  object list [--orphaned]\n"
"  object delete ID|PREFIX\n"
"  object delete --orphaned [--dry-run]\n"
"\n"
"Metadata:\n"
"  attr set ID KEY VALUE\n"
"  attr get ID\n"
"  attr remove ID KEY\n"
"  tag add ID TAG\n"
"  tag remove ID TAG\n"
"  tag list ID\n"
"  find --tag TAG | --attr KEY[=VALUE]\n"
"\n"
"Mounting:\n"
"  mount NAME [--ro] [--foreground] [--verbose] MOUNTPOINT\n"
"  unmount MOUNTPOINT\n"
"\n"
"Diagnostics:\n"
"  check [--fix] [--fill-checksums] [--verify-checksums] [--verbose]\n"
"\n"
"Global flags:\n"
"  --store PATH         backing store directory\n"
"                       (or set VIEWFS_STORE in the environment)\n"
"  --help, -h           show this message\n"
"  --version, -V        print the version\n",
        viewfs_version_string());
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }

    /* Pre-consume --store / --store=PATH so it can appear before the
     * subcommand name. The value is stashed in VIEWFS_STORE for the
     * downstream cli_open_store() call. */
    const char *store = cli_take_flag(&argc, argv, "--store", 0);
    if (store) setenv("VIEWFS_STORE", store, 1);

    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("viewfs %s\n", viewfs_version_string());
        return 0;
    }

    if (!strcmp(cmd, "init"))    return cmd_init   (argc, argv);
    if (!strcmp(cmd, "status"))  return cmd_status (argc, argv);
    if (!strcmp(cmd, "view"))    return cmd_view   (argc, argv);
    if (!strcmp(cmd, "object"))  return cmd_object (argc, argv);
    if (!strcmp(cmd, "attr"))    return cmd_attr   (argc, argv);
    if (!strcmp(cmd, "tag"))     return cmd_tag    (argc, argv);
    if (!strcmp(cmd, "find"))    return cmd_find   (argc, argv);
    if (!strcmp(cmd, "mount"))   return cmd_mount  (argc, argv);
    if (!strcmp(cmd, "unmount")) return cmd_unmount(argc, argv);
    if (!strcmp(cmd, "check"))   return cmd_check  (argc, argv);

    fprintf(stderr, "viewfs: unknown command '%s'\n", cmd);
    fprintf(stderr, "Run 'viewfs --help' for usage.\n");
    return 2;
}
