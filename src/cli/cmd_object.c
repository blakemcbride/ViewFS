#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* getuid, getgid */

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"Usage: viewfs object <subcommand> [args]\n"
"  object import HOST_PATH [--into VIEW:PATH]...\n"
"  object show ID|PREFIX\n"
"  object paths ID|PREFIX\n"
"  object id VIEW VIEW_PATH\n"
"  object list [--orphaned]\n"
"  object delete ID|PREFIX\n"
"  object delete --orphaned [--dry-run]\n");
}
static int usage(void) { print_usage(stderr); return 2; }

static void print_object_info(const vfs_object_info *o, void *ud) {
    (void)ud;
    printf("%s  %-7s  %10lld  mode=%04o  %s\n",
           o->id.hex, o->kind, (long long)o->size, o->mode,
           o->source_path ? o->source_path : "-");
}

static void print_mapping_for_obj(const vfs_mapping_row *m, void *ud) {
    (void)ud;
    printf("  %s:%s\n", m->view_name, m->view_path);
}

static int sub_import(int argc, char **argv, vfs_store *s) {
    /* Collect --into flags before consuming positional arg. */
    const char *intos[16] = { NULL };
    int nintos = 0;
    for (;;) {
        const char *t = cli_take_flag(&argc, argv, "--into", 0);
        if (!t) break;
        if (nintos >= 16) {
            fprintf(stderr, "viewfs: too many --into flags (max 16)\n");
            return 2;
        }
        intos[nintos++] = t;
    }
    if (argc != 4) return usage();
    const char *host_path = argv[3];

    vfs_object_id id;
    vfs_error rc = vfs_object_id_generate(&id);
    if (rc != VFS_OK) return cli_perror(s, rc, "object import");

    int64_t size = 0, mt = 0;
    int mode = 0644;
    char  checksum_hex[65];
    unsigned char checksum_state[128];
    size_t checksum_state_len = 0;
    rc = vfs_content_import_host(s, host_path, &id, &size, &mode, &mt,
                                 checksum_hex,
                                 checksum_state, &checksum_state_len);
    if (rc != VFS_OK) return cli_perror(s, rc, "object import");

    int64_t now = vfs_now_ns();
    if (!mt) mt = now;
    rc = vfs_object_insert_existing(s, &id, "file", mode, size,
                                    (int)getuid(), (int)getgid(),
                                    checksum_hex,
                                    checksum_state, checksum_state_len,
                                    now, mt, now, host_path);
    if (rc != VFS_OK) {
        vfs_content_unlink(s, &id);
        return cli_perror(s, rc, "object import");
    }

    for (int i = 0; i < nintos; i++) {
        const char *spec = intos[i];
        const char *colon = strchr(spec, ':');
        if (!colon || colon == spec || !colon[1]) {
            fprintf(stderr, "viewfs: --into expects VIEW:PATH (got '%s')\n", spec);
            continue;
        }
        size_t vlen = (size_t)(colon - spec);
        char view[128];
        if (vlen >= sizeof view) {
            fprintf(stderr, "viewfs: view name too long: %s\n", spec);
            continue;
        }
        memcpy(view, spec, vlen); view[vlen] = '\0';
        rc = vfs_mapping_add_file(s, view, colon + 1, &id);
        if (rc != VFS_OK) {
            fprintf(stderr, "viewfs: --into %s: %s (%s)\n",
                    spec, vfs_error_str(rc), vfs_store_last_error(s));
        } else {
            printf("  -> %s:%s\n", view, colon + 1);
        }
    }
    printf("%s  imported from %s\n", id.hex, host_path);
    return 0;
}

static int sub_show(int argc, char **argv, vfs_store *s) {
    if (argc != 4) return usage();
    vfs_object_id id;
    vfs_error rc = vfs_object_resolve(s, argv[3], &id);
    if (rc != VFS_OK) {
        if (rc == VFS_ERR_NOTFOUND)
            fprintf(stderr, "viewfs: no object matching '%s'\n", argv[3]);
        else if (rc == VFS_ERR_AMBIGUOUS)
            fprintf(stderr, "viewfs: prefix '%s' is ambiguous\n", argv[3]);
        else return cli_perror(s, rc, "object show");
        return 1;
    }
    vfs_object_info info;
    rc = vfs_object_get(s, &id, &info);
    if (rc != VFS_OK) return cli_perror(s, rc, "object show");
    printf("object %s\n", info.id.hex);
    printf("  kind:        %s\n", info.kind);
    printf("  size:        %lld\n", (long long)info.size);
    printf("  mode:        %04o\n", info.mode);
    if (info.uid >= 0) printf("  uid:         %d\n", info.uid);
    else               printf("  uid:         -\n");
    if (info.gid >= 0) printf("  gid:         %d\n", info.gid);
    else               printf("  gid:         -\n");
    printf("  source_path: %s\n", info.source_path ? info.source_path : "-");
    printf("  checksum:    %s%s\n",
           info.checksum ? info.checksum : "-",
           info.has_checksum_state ? " (resumable)" : "");
    printf("  ctime_ns:    %lld\n", (long long)info.ctime_ns);
    printf("  mtime_ns:    %lld\n", (long long)info.mtime_ns);
    return 0;
}

static int sub_paths(int argc, char **argv, vfs_store *s) {
    if (argc != 4) return usage();
    vfs_object_id id;
    vfs_error rc = vfs_object_resolve(s, argv[3], &id);
    if (rc != VFS_OK) {
        if (rc == VFS_ERR_NOTFOUND)
            fprintf(stderr, "viewfs: no object matching '%s'\n", argv[3]);
        else if (rc == VFS_ERR_AMBIGUOUS)
            fprintf(stderr, "viewfs: prefix '%s' is ambiguous\n", argv[3]);
        else return cli_perror(s, rc, "object paths");
        return 1;
    }
    printf("object %s:\n", id.hex);
    rc = vfs_mapping_list_object(s, &id, print_mapping_for_obj, NULL);
    if (rc != VFS_OK) return cli_perror(s, rc, "object paths");
    return 0;
}

static int sub_id(int argc, char **argv, vfs_store *s) {
    /* argv[0]=viewfs, argv[1]=object, argv[2]=id, argv[3]=VIEW, argv[4]=PATH */
    if (argc != 5) return usage();
    const char *view = argv[3];
    const char *path = argv[4];
    vfs_object_id id;
    vfs_error rc = vfs_mapping_object_id(s, view, path, &id);
    if (rc == VFS_ERR_NOTFOUND) {
        fprintf(stderr, "viewfs: no mapping at %s:%s\n", view, path);
        return 1;
    }
    if (rc == VFS_ERR_ISDIR) {
        fprintf(stderr, "viewfs: %s:%s is a directory (no object id)\n",
                view, path);
        return 1;
    }
    if (rc != VFS_OK) return cli_perror(s, rc, "object id");
    printf("%s\n", id.hex);
    return 0;
}

static int sub_list(int argc, char **argv, vfs_store *s) {
    const char *orphaned = cli_take_flag(&argc, argv, "--orphaned", 1);
    if (argc != 3) return usage();
    vfs_error rc = orphaned
        ? vfs_object_list_orphans(s, print_object_info, NULL)
        : vfs_object_list(s, print_object_info, NULL);
    if (rc != VFS_OK) return cli_perror(s, rc, "object list");
    return 0;
}

/* Bulk-orphan-delete state, shared between the iterator callback and the
 * top-level sub_delete handler. */
struct orphan_delete_ctx {
    vfs_store *store;
    int        dry_run;
    int        deleted;
    int        failed;
};

static void orphan_delete_cb(const vfs_object_info *o, void *ud) {
    struct orphan_delete_ctx *c = ud;
    if (c->dry_run) {
        printf("would delete %s  (%s)\n", o->id.hex,
               o->source_path ? o->source_path : "no source path");
        c->deleted++;
        return;
    }
    vfs_error rc = vfs_object_delete(c->store, &o->id);
    if (rc == VFS_OK) {
        printf("deleted %s\n", o->id.hex);
        c->deleted++;
    } else {
        fprintf(stderr, "viewfs: failed to delete %s: %s (%s)\n",
                o->id.hex, vfs_error_str(rc), vfs_store_last_error(c->store));
        c->failed++;
    }
}

static int sub_delete(int argc, char **argv, vfs_store *s) {
    const char *orphaned = cli_take_flag(&argc, argv, "--orphaned", 1);
    const char *dry_run  = cli_take_flag(&argc, argv, "--dry-run",  1);

    if (orphaned) {
        if (argc != 3) {
            fprintf(stderr,
                "viewfs: object delete --orphaned does not take an "
                "object id\n");
            return 2;
        }
        struct orphan_delete_ctx c = {
            .store = s, .dry_run = dry_run != NULL,
        };
        vfs_error rc = vfs_object_list_orphans(s, orphan_delete_cb, &c);
        if (rc != VFS_OK) return cli_perror(s, rc, "object delete --orphaned");
        const char *verb = c.dry_run ? "would delete" : "deleted";
        printf("%s %d orphan(s)%s%s\n",
               verb, c.deleted,
               c.failed ? ", " : "",
               c.failed ? "with failures" : "");
        if (c.failed) printf("  %d failure(s) -- see lines above\n", c.failed);
        return c.failed ? 1 : 0;
    }

    if (dry_run) {
        fprintf(stderr,
            "viewfs: --dry-run only applies with --orphaned\n");
        return 2;
    }
    if (argc != 4) return usage();
    vfs_object_id id;
    vfs_error rc = vfs_object_resolve(s, argv[3], &id);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs: %s\n", vfs_error_str(rc));
        return 1;
    }
    rc = vfs_object_delete(s, &id);
    if (rc != VFS_OK) return cli_perror(s, rc, "object delete");
    printf("Deleted object %s\n", id.hex);
    return 0;
}

int cmd_object(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *sub = argv[2];
    if (cli_is_help_request(sub)) { print_usage(stdout); return 0; }

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    int rc;
    if      (!strcmp(sub, "import")) rc = sub_import(argc, argv, s);
    else if (!strcmp(sub, "show"))   rc = sub_show  (argc, argv, s);
    else if (!strcmp(sub, "paths"))  rc = sub_paths (argc, argv, s);
    else if (!strcmp(sub, "id"))     rc = sub_id    (argc, argv, s);
    else if (!strcmp(sub, "list"))   rc = sub_list  (argc, argv, s);
    else if (!strcmp(sub, "delete")) rc = sub_delete(argc, argv, s);
    else { vfs_store_close(s); return usage(); }

    vfs_store_close(s);
    return rc;
}
