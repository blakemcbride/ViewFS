#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/* Append `key='value'` to dst (with a leading space if dst is non-empty),
 * escaping single quotes and backslashes for libpq's conninfo grammar.
 * Returns 0 on success, -1 if the result wouldn't fit in dstsz. */
static int append_conn_kv(char *dst, size_t dstsz,
                          const char *key, const char *value) {
    size_t pos = strlen(dst);
    int    sep = pos > 0;
    size_t need = (sep ? 1 : 0) + strlen(key) + 3 + strlen(value) * 2 + 1;
    if (pos + need >= dstsz) return -1;
    if (sep) dst[pos++] = ' ';
    size_t klen = strlen(key);
    memcpy(dst + pos, key, klen);
    pos += klen;
    dst[pos++] = '=';
    dst[pos++] = '\'';
    for (const char *p = value; *p; p++) {
        if (*p == '\'' || *p == '\\') dst[pos++] = '\\';
        dst[pos++] = *p;
    }
    dst[pos++] = '\'';
    dst[pos]   = '\0';
    return 0;
}

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
"  --pg CONNINFO libpq connection string. If omitted, the conninfo is\n"
"                built from VIEWFS_PG_USER and VIEWFS_PG_DATABASE (each\n"
"                optional); any field not supplied falls through to\n"
"                libpq's PG* env vars / compiled-in defaults.\n"
"  --schema NAME Postgres schema name. Defaults to 'viewfs'.\n"
"  --reinit      allow overwriting an existing config.toml.\n");
        return 2;
    }
    const char *store_path = argv[2];

    /* If --pg wasn't given, synthesize one from VIEWFS_PG_USER / _DATABASE.
     * Either or both may be unset, in which case libpq's own defaults apply
     * for the missing fields at open time. */
    char built[1024];
    built[0] = '\0';
    if (!conninfo) {
        const char *pg_user = getenv("VIEWFS_PG_USER");
        const char *pg_db   = getenv("VIEWFS_PG_DATABASE");
        if (pg_user && *pg_user &&
            append_conn_kv(built, sizeof built, "user", pg_user) < 0) {
            fprintf(stderr, "viewfs init: VIEWFS_PG_USER too long\n");
            return 1;
        }
        if (pg_db && *pg_db &&
            append_conn_kv(built, sizeof built, "dbname", pg_db) < 0) {
            fprintf(stderr, "viewfs init: VIEWFS_PG_DATABASE too long\n");
            return 1;
        }
        if (built[0]) conninfo = built;
    }

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
