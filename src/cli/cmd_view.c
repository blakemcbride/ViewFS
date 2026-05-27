#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"Usage: viewfs view <subcommand> [args]\n"
"  view create NAME [\"DESCRIPTION\"]\n"
"  view list\n"
"  view show NAME\n"
"  view delete NAME\n"
"  view add VIEW OBJECT_ID|PREFIX VIEW_PATH\n"
"  view remove VIEW VIEW_PATH\n"
"  view populate VIEW --tag TAG --under VIEW_PATH\n");
}
static int usage(void) { print_usage(stderr); return 2; }

static void print_view_row(const vfs_view_row *r, void *ud) {
    (void)ud;
    printf("%-24s %s\n", r->name, r->description ? r->description : "");
}

static void print_mapping_row(const vfs_mapping_row *r, void *ud) {
    (void)ud;
    if (r->has_object) {
        printf("  %-6s %-40s -> %s\n", r->entry_kind, r->view_path,
               r->object_id.hex);
    } else {
        printf("  %-6s %s\n", r->entry_kind, r->view_path);
    }
}

static int sub_create(int argc, char **argv, vfs_store *s) {
    /* argv: viewfs, view, create, NAME, [DESCRIPTION] */
    if (argc != 4 && argc != 5) { return usage(); }
    const char *name = argv[3];
    const char *desc = (argc == 5) ? argv[4] : NULL;
    vfs_error rc = vfs_view_create(s, name, desc);
    if (rc == VFS_ERR_EXISTS) {
        fprintf(stderr, "viewfs: view '%s' already exists\n", name);
        return 1;
    }
    if (rc != VFS_OK) return cli_perror(s, rc, "view create");
    printf("Created view '%s'\n", name);
    return 0;
}

static int sub_list(int argc, char **argv, vfs_store *s) {
    (void)argv;
    if (argc != 3) return usage();
    vfs_error rc = vfs_view_list(s, print_view_row, NULL);
    if (rc != VFS_OK) return cli_perror(s, rc, "view list");
    return 0;
}

static int sub_show(int argc, char **argv, vfs_store *s) {
    if (argc != 4) return usage();
    const char *name = argv[3];
    int exists = 0;
    vfs_error rc = vfs_view_exists(s, name, &exists);
    if (rc != VFS_OK) return cli_perror(s, rc, "view show");
    if (!exists) {
        fprintf(stderr, "viewfs: view '%s' does not exist\n", name);
        return 1;
    }
    printf("view '%s':\n", name);
    rc = vfs_mapping_list_view(s, name, print_mapping_row, NULL);
    if (rc != VFS_OK) return cli_perror(s, rc, "view show");
    return 0;
}

static int sub_delete(int argc, char **argv, vfs_store *s) {
    if (argc != 4) return usage();
    const char *name = argv[3];
    vfs_error rc = vfs_view_delete(s, name);
    if (rc == VFS_ERR_NOTFOUND) {
        fprintf(stderr, "viewfs: view '%s' does not exist\n", name);
        return 1;
    }
    if (rc != VFS_OK) return cli_perror(s, rc, "view delete");
    printf("Deleted view '%s'\n", name);
    return 0;
}

static int sub_add(int argc, char **argv, vfs_store *s) {
    if (argc != 6) return usage();
    const char *view = argv[3];
    const char *id_str = argv[4];
    const char *path = argv[5];

    vfs_object_id id;
    vfs_error rc = vfs_object_resolve(s, id_str, &id);
    if (rc == VFS_ERR_NOTFOUND) {
        fprintf(stderr, "viewfs: no object matching '%s'\n", id_str);
        return 1;
    }
    if (rc == VFS_ERR_AMBIGUOUS) {
        fprintf(stderr, "viewfs: object prefix '%s' is ambiguous\n", id_str);
        return 1;
    }
    if (rc != VFS_OK) return cli_perror(s, rc, "view add");

    rc = vfs_mapping_add_file(s, view, path, &id);
    if (rc == VFS_ERR_EXISTS) {
        fprintf(stderr, "viewfs: mapping %s:%s already exists\n", view, path);
        return 1;
    }
    if (rc != VFS_OK) return cli_perror(s, rc, "view add");
    printf("%s:%s -> %s\n", view, path, id.hex);
    return 0;
}

/* `viewfs view populate VIEW --tag TAG --under VIEW_PATH` */
struct populate_ctx {
    vfs_store  *store;
    const char *view;
    const char *under;       /* canonical, with leading '/' */
    int         added;
    int         skipped;
    int         failed;
};

static const char *basename_of(const char *p) {
    if (!p) return NULL;
    const char *slash = strrchr(p, '/');
    if (!slash) return p;
    return slash[1] ? slash + 1 : NULL;
}

static void populate_cb(const vfs_object_info *o, void *ud) {
    struct populate_ctx *pc = ud;
    const char *base = basename_of(o->source_path);
    if (!base) base = o->id.hex;
    char vp[VFS_PATH_MAX];
    int n = snprintf(vp, sizeof vp, "%s/%s",
                     pc->under[1] ? pc->under : "", base);
    if (n < 0 || (size_t)n >= sizeof vp) {
        fprintf(stderr, "viewfs: path too long for %s/%s\n", pc->under, base);
        pc->failed++;
        return;
    }
    vfs_error rc = vfs_mapping_add_file(pc->store, pc->view, vp, &o->id);
    if (rc == VFS_OK) {
        pc->added++;
        printf("  + %s:%s -> %s\n", pc->view, vp, o->id.hex);
    } else if (rc == VFS_ERR_EXISTS) {
        pc->skipped++;
        fprintf(stderr, "  - %s:%s already exists (skipped)\n", pc->view, vp);
    } else {
        pc->failed++;
        fprintf(stderr, "  ! %s:%s: %s (%s)\n", pc->view, vp,
                vfs_error_str(rc), vfs_store_last_error(pc->store));
    }
}

static int sub_populate(int argc, char **argv, vfs_store *s) {
    const char *tag   = cli_take_flag(&argc, argv, "--tag",   0);
    const char *under = cli_take_flag(&argc, argv, "--under", 0);
    if (!tag || !under || argc != 4) return usage();
    const char *view = argv[3];

    int exists = 0;
    vfs_error vrc = vfs_view_exists(s, view, &exists);
    if (vrc != VFS_OK) return cli_perror(s, vrc, "view populate");
    if (!exists) {
        fprintf(stderr, "viewfs: view '%s' does not exist\n", view);
        return 1;
    }

    vfs_canon_path cp;
    vrc = vfs_path_canonicalize(under, &cp);
    if (vrc != VFS_OK) {
        fprintf(stderr,
            "viewfs: --under '%s' is not a valid view path (%s)\n",
            under, vfs_error_str(vrc));
        return 2;
    }

    struct populate_ctx pc = {
        .store = s, .view = view, .under = cp.path,
        .added = 0, .skipped = 0, .failed = 0,
    };
    vrc = vfs_find_by_tag(s, tag, populate_cb, &pc);
    if (vrc != VFS_OK) return cli_perror(s, vrc, "view populate");

    printf("Populated %d, skipped %d, failed %d (tag '%s' into %s under %s)\n",
           pc.added, pc.skipped, pc.failed, tag, view, cp.path);
    return pc.failed ? 1 : 0;
}

static int sub_remove(int argc, char **argv, vfs_store *s) {
    if (argc != 5) return usage();
    const char *view = argv[3];
    const char *path = argv[4];
    vfs_error rc = vfs_mapping_remove(s, view, path);
    if (rc == VFS_ERR_NOTFOUND) {
        fprintf(stderr, "viewfs: no mapping at %s:%s\n", view, path);
        return 1;
    }
    if (rc == VFS_ERR_NOTEMPTY) {
        fprintf(stderr, "viewfs: directory %s:%s is not empty\n", view, path);
        return 1;
    }
    if (rc != VFS_OK) return cli_perror(s, rc, "view remove");
    printf("Removed %s:%s\n", view, path);
    return 0;
}

int cmd_view(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *sub = argv[2];
    if (cli_is_help_request(sub)) { print_usage(stdout); return 0; }

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    int rc;
    if      (!strcmp(sub, "create")) rc = sub_create(argc, argv, s);
    else if (!strcmp(sub, "list"))   rc = sub_list  (argc, argv, s);
    else if (!strcmp(sub, "show"))   rc = sub_show  (argc, argv, s);
    else if (!strcmp(sub, "delete")) rc = sub_delete(argc, argv, s);
    else if (!strcmp(sub, "add"))    rc = sub_add   (argc, argv, s);
    else if (!strcmp(sub, "remove")) rc = sub_remove(argc, argv, s);
    else if (!strcmp(sub, "populate")) rc = sub_populate(argc, argv, s);
    else { vfs_store_close(s); return usage(); }

    vfs_store_close(s);
    return rc;
}
