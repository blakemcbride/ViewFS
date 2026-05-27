#include <stdio.h>
#include <string.h>

#include "common.h"

static int usage(void) {
    fprintf(stderr,
"Usage: viewfs find --tag TAG\n"
"       viewfs find --attr KEY=VALUE\n"
"       viewfs find --attr KEY\n");
    return 2;
}

static void print_object(const vfs_object_info *o, void *ud) {
    (void)ud;
    printf("%s  %-7s  %10lld  %s\n",
           o->id.hex, o->kind, (long long)o->size,
           o->source_path ? o->source_path : "");
}

int cmd_find(int argc, char **argv) {
    const char *tag  = cli_take_flag(&argc, argv, "--tag", 0);
    const char *attr = cli_take_flag(&argc, argv, "--attr", 0);
    if (!tag && !attr) return usage();
    if (tag && attr) {
        fprintf(stderr, "viewfs: --tag and --attr are mutually exclusive\n");
        return 2;
    }
    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    vfs_error e;
    if (tag) {
        e = vfs_find_by_tag(s, tag, print_object, NULL);
    } else {
        char key[256];
        const char *eq = strchr(attr, '=');
        if (!eq) {
            e = vfs_find_by_attr(s, attr, NULL, print_object, NULL);
        } else {
            size_t kl = (size_t)(eq - attr);
            if (kl >= sizeof key) { vfs_store_close(s); return usage(); }
            memcpy(key, attr, kl); key[kl] = '\0';
            e = vfs_find_by_attr(s, key, eq + 1, print_object, NULL);
        }
    }
    int rc = (e == VFS_OK) ? 0 : cli_perror(s, e, "find");
    vfs_store_close(s);
    return rc;
}
