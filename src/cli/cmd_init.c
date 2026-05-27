#include <stdio.h>
#include <string.h>

#include "common.h"

int cmd_init(int argc, char **argv) {
    /* argv[0]="viewfs", argv[1]="init", argv[2]=STORE_PATH, then flags */
    const char *conninfo = cli_take_flag(&argc, argv, "--pg", 0);
    const char *schema   = cli_take_flag(&argc, argv, "--schema", 0);
    const char *reinit_s = cli_take_flag(&argc, argv, "--reinit", 1);
    int reinit = reinit_s != NULL;

    if (argc != 3) {
        fprintf(stderr,
"Usage: viewfs init STORE_PATH [--pg CONNINFO] [--schema NAME] [--reinit]\n"
"\n"
"  STORE_PATH    directory to hold config + content blobs\n"
"  --pg CONNINFO libpq connection string. Defaults to env (PGHOST etc.)\n"
"  --schema NAME Postgres schema name. Defaults to 'viewfs'.\n"
"  --reinit      allow overwriting an existing config.toml.\n");
        return 2;
    }
    const char *store_path = argv[2];

    vfs_error rc = vfs_store_create(store_path, conninfo, schema, reinit);
    if (rc == VFS_ERR_EXISTS) {
        fprintf(stderr,
            "viewfs: %s already initialized "
            "(pass --reinit to overwrite config.toml)\n", store_path);
        return 1;
    }
    if (rc != VFS_OK) {
        /* vfs_store_create has already printed a detail line. */
        fprintf(stderr, "viewfs init failed: %s\n", vfs_error_str(rc));
        return 1;
    }
    printf("Initialized ViewFS store at %s\n", store_path);
    return 0;
}
