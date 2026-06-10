#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"vfs %s -- view-based filesystem admin tool\n"
"\n"
"Usage: vfs <command> [options...]\n"
"\n"
"Repository:\n"
"  init [STORE_PATH] [--pg CONNINFO] [--schema NAME] [--reinit]\n"
"  status                           show store status\n"
"\n"
"Views:\n"
"  view create NAME [\"DESCRIPTION\"]\n"
"  view list\n"
"  view show NAME                   list the view's directory tree\n"
"  view delete NAME\n"
"\n"
"Directories (per view; contents are computed from properties):\n"
"  (inside a mounted view, VIEW/DIR may be omitted -> taken from your cwd)\n"
"  dir mkdir [VIEW] DIR\n"
"  dir rmdir [VIEW] DIR\n"
"  dir ls [[VIEW] DIR]              list computed contents (dirs + files)\n"
"\n"
"Objects:\n"
"  object import HOST_PATH [--name NAME] [--into VIEW:DIR]...\n"
"  object show ID|PREFIX\n"
"  object name ID|PREFIX [NEWNAME]\n"
"  object copy ID|PREFIX VIEW:DIR\n"
"  object id VIEW VIEW_PATH\n"
"  object list [--orphaned]\n"
"  object delete ID|PREFIX\n"
"  object delete --orphaned [--dry-run]\n"
"\n"
"Properties (on a file OR a directory's membership filter; multi-valued):\n"
"  prop set    [TARGET] KEY VALUE [--flow]\n"
"  prop unset  [TARGET] KEY [VALUE]\n"
"  prop list   [TARGET] [--effective]\n"
"  find --prop KEY[=VALUE] [--prop KEY[=VALUE]]...\n"
"  TARGET: a path/name in your mount, VIEW:DIR, an object ID, or omitted=cwd\n"
"  (--flow/--effective apply to directory targets; a tag is key 'tag')\n"
"\n"
"Mounting:\n"
"  mount NAME [--ro] [--foreground] [--verbose] MOUNTPOINT\n"
"  mounts                           list mounted views and their mountpoints\n"
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
        printf("vfs %s\n", viewfs_version_string());
        return 0;
    }

    if (!strcmp(cmd, "init"))    return cmd_init   (argc, argv);
    if (!strcmp(cmd, "status"))  return cmd_status (argc, argv);
    if (!strcmp(cmd, "view"))    return cmd_view   (argc, argv);
    if (!strcmp(cmd, "dir"))     return cmd_dir    (argc, argv);
    if (!strcmp(cmd, "object"))  return cmd_object (argc, argv);
    if (!strcmp(cmd, "prop"))    return cmd_prop   (argc, argv);
    if (!strcmp(cmd, "find"))    return cmd_find   (argc, argv);
    if (!strcmp(cmd, "mount"))   return cmd_mount  (argc, argv);
    if (!strcmp(cmd, "mounts"))  return cmd_mounts (argc, argv);
    if (!strcmp(cmd, "unmount")) return cmd_unmount(argc, argv);
    if (!strcmp(cmd, "check"))   return cmd_check  (argc, argv);

    fprintf(stderr, "vfs: unknown command '%s'\n", cmd);
    fprintf(stderr, "Run 'vfs --help' for usage.\n");
    return 2;
}
