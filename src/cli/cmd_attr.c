#include <stdio.h>
#include <string.h>

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"Usage: viewfs attr <subcommand>\n"
"  attr set ID|PREFIX KEY VALUE\n"
"  attr get ID|PREFIX\n"
"  attr remove ID|PREFIX KEY\n");
}
static int usage(void) { print_usage(stderr); return 2; }

static void print_attr(const vfs_attr_row *a, void *ud) {
    (void)ud;
    printf("  %-24s %s\n", a->key, a->value);
}

static int resolve_or_die(vfs_store *s, const char *arg, vfs_object_id *id) {
    vfs_error rc = vfs_object_resolve(s, arg, id);
    if (rc == VFS_OK) return 0;
    if (rc == VFS_ERR_NOTFOUND)
        fprintf(stderr, "viewfs: no object matching '%s'\n", arg);
    else if (rc == VFS_ERR_AMBIGUOUS)
        fprintf(stderr, "viewfs: prefix '%s' is ambiguous\n", arg);
    else
        fprintf(stderr, "viewfs: %s (%s)\n",
                vfs_error_str(rc), vfs_store_last_error(s));
    return 1;
}

int cmd_attr(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *sub = argv[2];
    if (cli_is_help_request(sub)) { print_usage(stdout); return 0; }

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    int rc = 0;
    if (!strcmp(sub, "set")) {
        if (argc != 6) { vfs_store_close(s); return usage(); }
        vfs_object_id id;
        if (resolve_or_die(s, argv[3], &id)) { vfs_store_close(s); return 1; }
        vfs_error e = vfs_attr_set(s, &id, argv[4], argv[5]);
        if (e != VFS_OK) rc = cli_perror(s, e, "attr set");
        else printf("Set %s.%s = %s\n", id.hex, argv[4], argv[5]);
    } else if (!strcmp(sub, "get")) {
        if (argc != 4) { vfs_store_close(s); return usage(); }
        vfs_object_id id;
        if (resolve_or_die(s, argv[3], &id)) { vfs_store_close(s); return 1; }
        printf("attributes of %s:\n", id.hex);
        vfs_error e = vfs_attr_get(s, &id, print_attr, NULL);
        if (e != VFS_OK) rc = cli_perror(s, e, "attr get");
    } else if (!strcmp(sub, "remove")) {
        if (argc != 5) { vfs_store_close(s); return usage(); }
        vfs_object_id id;
        if (resolve_or_die(s, argv[3], &id)) { vfs_store_close(s); return 1; }
        vfs_error e = vfs_attr_remove(s, &id, argv[4]);
        if (e == VFS_ERR_NOTFOUND) {
            fprintf(stderr, "viewfs: no attribute %s on %s\n", argv[4], id.hex);
            rc = 1;
        } else if (e != VFS_OK) rc = cli_perror(s, e, "attr remove");
        else printf("Removed %s.%s\n", id.hex, argv[4]);
    } else {
        vfs_store_close(s);
        return usage();
    }
    vfs_store_close(s);
    return rc;
}
