#include <stdio.h>
#include <string.h>

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"Usage: viewfs tag <subcommand>\n"
"  tag add ID|PREFIX TAG\n"
"  tag remove ID|PREFIX TAG\n"
"  tag list ID|PREFIX\n");
}
static int usage(void) { print_usage(stderr); return 2; }

static void print_tag(const char *tag, int64_t ctime_ns, void *ud) {
    (void)ud; (void)ctime_ns;
    printf("  %s\n", tag);
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

int cmd_tag(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *sub = argv[2];
    if (cli_is_help_request(sub)) { print_usage(stdout); return 0; }

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    int rc = 0;
    if (!strcmp(sub, "add")) {
        if (argc != 5) { vfs_store_close(s); return usage(); }
        vfs_object_id id;
        if (resolve_or_die(s, argv[3], &id)) { vfs_store_close(s); return 1; }
        vfs_error e = vfs_tag_add(s, &id, argv[4]);
        if (e != VFS_OK) rc = cli_perror(s, e, "tag add");
        else printf("Added tag '%s' to %s\n", argv[4], id.hex);
    } else if (!strcmp(sub, "remove")) {
        if (argc != 5) { vfs_store_close(s); return usage(); }
        vfs_object_id id;
        if (resolve_or_die(s, argv[3], &id)) { vfs_store_close(s); return 1; }
        vfs_error e = vfs_tag_remove(s, &id, argv[4]);
        if (e == VFS_ERR_NOTFOUND) {
            fprintf(stderr, "viewfs: %s does not have tag '%s'\n", id.hex, argv[4]);
            rc = 1;
        } else if (e != VFS_OK) rc = cli_perror(s, e, "tag remove");
        else printf("Removed tag '%s' from %s\n", argv[4], id.hex);
    } else if (!strcmp(sub, "list")) {
        if (argc != 4) { vfs_store_close(s); return usage(); }
        vfs_object_id id;
        if (resolve_or_die(s, argv[3], &id)) { vfs_store_close(s); return 1; }
        printf("tags of %s:\n", id.hex);
        vfs_error e = vfs_tag_list(s, &id, print_tag, NULL);
        if (e != VFS_OK) rc = cli_perror(s, e, "tag list");
    } else {
        vfs_store_close(s);
        return usage();
    }
    vfs_store_close(s);
    return rc;
}
