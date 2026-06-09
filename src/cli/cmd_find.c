/* `viewfs find --prop KEY[=VALUE] ...` -- objects matching ALL pairs (AND).
 * A bare KEY matches any value of that key. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define MAX_PAIRS 32

static int usage(void) {
    fprintf(stderr,
"Usage: viewfs find --prop KEY[=VALUE] [--prop KEY[=VALUE]]...\n"
"  Lists objects whose properties satisfy every --prop pair.\n"
"  A bare KEY (no =VALUE) matches any value of that key.\n");
    return 2;
}

static void print_object(const vfs_object_info *o, void *ud) {
    (void)ud;
    printf("%s  %-7s  %-24s %10lld\n",
           o->id.hex, o->kind, o->name[0] ? o->name : "-", (long long)o->size);
}

int cmd_find(int argc, char **argv) {
    /* Collect every --prop occurrence. */
    char *specs[MAX_PAIRS];
    int nspecs = 0;
    for (;;) {
        const char *p = cli_take_flag(&argc, argv, "--prop", 0);
        if (!p) break;
        if (nspecs >= MAX_PAIRS) {
            fprintf(stderr, "viewfs: too many --prop flags (max %d)\n", MAX_PAIRS);
            return 2;
        }
        specs[nspecs] = strdup(p);
        if (!specs[nspecs]) { fprintf(stderr, "viewfs: out of memory\n"); return 2; }
        nspecs++;
    }
    if (nspecs == 0) { for (int i = 0; i < nspecs; i++) free(specs[i]); return usage(); }

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) { for (int i = 0; i < nspecs; i++) free(specs[i]); return rc_open; }

    if (argc != 2) {
        for (int i = 0; i < nspecs; i++) free(specs[i]);
        vfs_store_close(s);
        return usage();
    }

    vfs_prop_pair pairs[MAX_PAIRS];
    for (int i = 0; i < nspecs; i++) {
        char *eq = strchr(specs[i], '=');
        if (eq) { *eq = '\0'; pairs[i].key = specs[i]; pairs[i].value = eq + 1; }
        else    { pairs[i].key = specs[i]; pairs[i].value = NULL; }
    }

    vfs_error e = vfs_find_by_props(s, pairs, nspecs, print_object, NULL);
    int rc = (e == VFS_OK) ? 0 : cli_perror(s, e, "find");

    for (int i = 0; i < nspecs; i++) free(specs[i]);
    vfs_store_close(s);
    return rc;
}
