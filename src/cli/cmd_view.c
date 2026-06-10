#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"Usage: vfs view <subcommand> [args]\n"
"  view create NAME [\"DESCRIPTION\"]\n"
"  view list\n"
"  view show NAME                 list the view's directory tree\n"
"  view delete NAME\n"
"\n"
"  Use `vfs dir ...` to create directories and attach property\n"
"  filters; file membership is then computed from those properties.\n");
}
static int usage(void) { print_usage(stderr); return 2; }

static void print_view_row(const vfs_view_row *r, void *ud) {
    (void)ud;
    printf("%-24s %s\n", r->name, r->description ? r->description : "");
}

static int sub_create(int argc, char **argv, vfs_store *s) {
    if (argc != 4 && argc != 5) return usage();
    const char *name = argv[3];
    const char *desc = (argc == 5) ? argv[4] : NULL;
    vfs_error rc = vfs_view_create(s, name, desc);
    if (rc == VFS_ERR_EXISTS) {
        fprintf(stderr, "vfs: view '%s' already exists\n", name);
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

/* Recursively print the directory tree. The callback re-enters
 * vfs_dir_list_children for each child; safe because each PGresult is
 * fully materialized before the callback runs. */
struct tree_ctx { vfs_store *s; const char *view; int depth; };

static void print_tree_node(const vfs_dir_row *d, void *ud);

static void walk_children(vfs_store *s, const char *view, const char *dir,
                          int depth) {
    struct tree_ctx ctx = { .s = s, .view = view, .depth = depth };
    vfs_dir_list_children(s, view, dir, print_tree_node, &ctx);
}

static void print_tree_node(const vfs_dir_row *d, void *ud) {
    struct tree_ctx *ctx = ud;
    printf("%*s%s/\n", ctx->depth * 2, "", d->name);
    walk_children(ctx->s, ctx->view, d->dir_path, ctx->depth + 1);
}

static int sub_show(int argc, char **argv, vfs_store *s) {
    if (argc != 4) return usage();
    const char *name = argv[3];
    int exists = 0;
    vfs_error rc = vfs_view_exists(s, name, &exists);
    if (rc != VFS_OK) return cli_perror(s, rc, "view show");
    if (!exists) {
        fprintf(stderr, "vfs: view '%s' does not exist\n", name);
        return 1;
    }
    printf("view '%s':\n", name);
    printf("/\n");
    walk_children(s, name, "/", 1);
    return 0;
}

static int sub_delete(int argc, char **argv, vfs_store *s) {
    if (argc != 4) return usage();
    const char *name = argv[3];
    vfs_error rc = vfs_view_delete(s, name);
    if (rc == VFS_ERR_NOTFOUND) {
        fprintf(stderr, "vfs: view '%s' does not exist\n", name);
        return 1;
    }
    if (rc != VFS_OK) return cli_perror(s, rc, "view delete");
    printf("Deleted view '%s'\n", name);
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
    else { vfs_store_close(s); return usage(); }

    vfs_store_close(s);
    return rc;
}
