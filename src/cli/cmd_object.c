#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* getuid, getgid */

#include "common.h"

static void print_usage(FILE *out) {
    fprintf(out,
"Usage: viewfs object <subcommand> [args]\n"
"  object import HOST_PATH [--name NAME] [--into VIEW:DIR]...\n"
"  object show ID|PREFIX\n"
"  object name ID|PREFIX [NEWNAME]\n"
"  object copy ID|PREFIX VIEW:DIR\n"
"  object id VIEW VIEW_PATH\n"
"  object list [--orphaned]\n"
"  object delete ID|PREFIX\n"
"  object delete --orphaned [--dry-run]\n");
}
static int usage(void) { print_usage(stderr); return 2; }

static const char *basename_of(const char *p) {
    if (!p) return "";
    const char *slash = strrchr(p, '/');
    if (!slash) return p;
    return slash[1] ? slash + 1 : p;
}

static void print_object_info(const vfs_object_info *o, void *ud) {
    (void)ud;
    printf("%s  %-7s  %-24s %10lld  mode=%04o\n",
           o->id.hex, o->kind, o->name[0] ? o->name : "-",
           (long long)o->size, o->mode);
}

/* Split "VIEW:DIR" into view + dir. Returns 0 on success. */
static int split_view_dir(const char *spec, char *view, size_t vsz,
                          const char **dir_out) {
    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec || !colon[1]) return -1;
    size_t vlen = (size_t)(colon - spec);
    if (vlen >= vsz) return -1;
    memcpy(view, spec, vlen); view[vlen] = '\0';
    *dir_out = colon + 1;
    return 0;
}

/* Add every effective property pair of VIEW:DIR to the object. Auto-creates
 * the directory if it does not exist (mkdir -p). */
struct addprops_ctx { vfs_store *s; const vfs_object_id *id; int failed; };
static void addprop_cb(const vfs_dirprop_row *p, void *ud) {
    struct addprops_ctx *c = ud;
    if (vfs_object_prop_add(c->s, c->id, p->key, p->value) != VFS_OK) c->failed++;
}
static vfs_error assign_dir_props(vfs_store *s, const vfs_object_id *id,
                                  const char *view, const char *dir) {
    vfs_error e = vfs_dir_create(s, view, dir, 0755);
    if (e != VFS_OK && e != VFS_ERR_EXISTS) return e;
    struct addprops_ctx c = { s, id, 0 };
    e = vfs_dir_prop_list(s, view, dir, 1 /*effective*/, addprop_cb, &c);
    if (e != VFS_OK) return e;
    return c.failed ? VFS_ERR_DB : VFS_OK;
}

static int sub_import(int argc, char **argv, vfs_store *s) {
    const char *name_flag = cli_take_flag(&argc, argv, "--name", 0);
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
    const char *name = name_flag ? name_flag : basename_of(host_path);

    vfs_object_id id;
    vfs_error rc = vfs_object_id_generate(&id);
    if (rc != VFS_OK) return cli_perror(s, rc, "object import");

    int64_t size = 0, mt = 0;
    int mode = 0644;
    char  checksum_hex[65];
    unsigned char checksum_state[128];
    size_t checksum_state_len = 0;
    rc = vfs_content_import_host(s, host_path, &id, &size, &mode, &mt,
                                 checksum_hex, checksum_state, &checksum_state_len);
    if (rc != VFS_OK) return cli_perror(s, rc, "object import");

    int64_t now = vfs_now_ns();
    if (!mt) mt = now;
    rc = vfs_object_insert_existing(s, &id, "file", name, mode, size,
                                    (int)getuid(), (int)getgid(),
                                    checksum_hex, checksum_state, checksum_state_len,
                                    now, mt, now, host_path);
    if (rc != VFS_OK) {
        vfs_content_unlink(s, &id);
        return cli_perror(s, rc, "object import");
    }

    for (int i = 0; i < nintos; i++) {
        char view[128];
        const char *dir = NULL;
        if (split_view_dir(intos[i], view, sizeof view, &dir) != 0) {
            fprintf(stderr, "viewfs: --into expects VIEW:DIR (got '%s')\n", intos[i]);
            continue;
        }
        rc = assign_dir_props(s, &id, view, dir);
        if (rc != VFS_OK)
            fprintf(stderr, "viewfs: --into %s: %s (%s)\n",
                    intos[i], vfs_error_str(rc), vfs_store_last_error(s));
        else
            printf("  -> %s:%s (gained dir properties)\n", view, dir);
    }
    printf("%s  imported from %s as '%s'\n", id.hex, host_path, name);
    return 0;
}

static int sub_show(int argc, char **argv, vfs_store *s) {
    if (argc != 4) return usage();
    vfs_object_id id;
    if (cli_resolve_object(s, argv[3], &id)) return 1;
    vfs_object_info info;
    vfs_error rc = vfs_object_get(s, &id, &info);
    if (rc != VFS_OK) return cli_perror(s, rc, "object show");
    printf("object %s\n", info.id.hex);
    printf("  name:        %s\n", info.name[0] ? info.name : "-");
    printf("  kind:        %s\n", info.kind);
    printf("  size:        %lld\n", (long long)info.size);
    printf("  mode:        %04o\n", info.mode);
    printf("  uid:         %s",  info.uid >= 0 ? "" : "-\n");
    if (info.uid >= 0) printf("%d\n", info.uid);
    printf("  gid:         %s",  info.gid >= 0 ? "" : "-\n");
    if (info.gid >= 0) printf("%d\n", info.gid);
    printf("  source_path: %s\n", info.source_path ? info.source_path : "-");
    printf("  checksum:    %s%s\n",
           info.checksum ? info.checksum : "-",
           info.has_checksum_state ? " (resumable)" : "");
    printf("  ctime_ns:    %lld\n", (long long)info.ctime_ns);
    printf("  mtime_ns:    %lld\n", (long long)info.mtime_ns);
    return 0;
}

static int sub_name(int argc, char **argv, vfs_store *s) {
    if (argc != 4 && argc != 5) return usage();
    vfs_object_id id;
    if (cli_resolve_object(s, argv[3], &id)) return 1;
    if (argc == 4) {
        vfs_object_info info;
        vfs_error rc = vfs_object_get(s, &id, &info);
        if (rc != VFS_OK) return cli_perror(s, rc, "object name");
        printf("%s\n", info.name);
        return 0;
    }
    vfs_error rc = vfs_object_set_name(s, &id, argv[4]);
    if (rc != VFS_OK) return cli_perror(s, rc, "object name");
    printf("%s renamed to '%s'\n", id.hex, argv[4]);
    return 0;
}

struct copyprops_ctx { vfs_store *s; const vfs_object_id *dst; int failed; };
static void copyprop_cb(const vfs_prop_row *p, void *ud) {
    struct copyprops_ctx *c = ud;
    if (vfs_object_prop_add(c->s, c->dst, p->key, p->value) != VFS_OK) c->failed++;
}

/* D4 copy: duplicate content into a new object, keep the source's
 * properties, and additionally gain VIEW:DIR's effective property pairs. */
static int sub_copy(int argc, char **argv, vfs_store *s) {
    if (argc != 5) return usage();
    vfs_object_id src;
    if (cli_resolve_object(s, argv[3], &src)) return 1;
    char view[128];
    const char *dir = NULL;
    if (split_view_dir(argv[4], view, sizeof view, &dir) != 0) return usage();

    vfs_object_info info;
    vfs_error rc = vfs_object_get(s, &src, &info);
    if (rc != VFS_OK) return cli_perror(s, rc, "object copy");
    if (strcmp(info.kind, "file") != 0) {
        fprintf(stderr, "viewfs: object copy only supports file objects\n");
        return 1;
    }
    char srcpath[VFS_PATH_MAX];
    rc = vfs_content_path(s, &src, srcpath, sizeof srcpath);
    if (rc != VFS_OK) return cli_perror(s, rc, "object copy");

    char name[VFS_NAME_MAX + 1];
    snprintf(name, sizeof name, "%s", info.name);

    vfs_object_id dst;
    rc = vfs_object_id_generate(&dst);
    if (rc != VFS_OK) return cli_perror(s, rc, "object copy");
    int64_t size = 0, mt = 0; int mode = info.mode;
    char cs_hex[65]; unsigned char cs_state[128]; size_t cs_len = 0;
    rc = vfs_content_import_host(s, srcpath, &dst, &size, &mode, &mt,
                                 cs_hex, cs_state, &cs_len);
    if (rc != VFS_OK) return cli_perror(s, rc, "object copy");
    int64_t now = vfs_now_ns();
    if (!mt) mt = now;
    rc = vfs_object_insert_existing(s, &dst, "file", name, mode, size,
                                    (int)getuid(), (int)getgid(),
                                    cs_hex, cs_state, cs_len, now, mt, now,
                                    info.source_path);
    if (rc != VFS_OK) { vfs_content_unlink(s, &dst); return cli_perror(s, rc, "object copy"); }

    /* Keep the source's properties... */
    struct copyprops_ctx cc = { s, &dst, 0 };
    rc = vfs_object_prop_list(s, &src, copyprop_cb, &cc);
    if (rc != VFS_OK) return cli_perror(s, rc, "object copy");

    /* ...and gain the destination directory's effective pairs. */
    rc = assign_dir_props(s, &dst, view, dir);
    if (rc != VFS_OK) return cli_perror(s, rc, "object copy");
    printf("%s  copied from %s into %s:%s\n", dst.hex, src.hex, view, dir);
    return 0;
}

static int sub_id(int argc, char **argv, vfs_store *s) {
    if (argc != 5) return usage();
    const char *view = argv[3];
    const char *path = argv[4];
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(path, &cp);
    if (rc != VFS_OK) return cli_perror(s, rc, "object id");
    if (cp.is_root) {
        fprintf(stderr, "viewfs: %s:%s is a directory\n", view, path);
        return 1;
    }
    const char *parent = cp.parent[0] ? cp.parent : "/";
    vfs_object_id id;
    rc = vfs_dir_member_by_name(s, view, parent, cp.name, &id);
    if (rc == VFS_ERR_NOTFOUND) {
        fprintf(stderr, "viewfs: no file %s:%s\n", view, path);
        return 1;
    }
    if (rc == VFS_ERR_AMBIGUOUS) {
        fprintf(stderr, "viewfs: %s:%s is ambiguous (multiple matches)\n", view, path);
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

struct orphan_delete_ctx { vfs_store *store; int dry_run; int deleted; int failed; };

static void orphan_delete_cb(const vfs_object_info *o, void *ud) {
    struct orphan_delete_ctx *c = ud;
    if (c->dry_run) {
        printf("would delete %s  (%s)\n", o->id.hex, o->name[0] ? o->name : "-");
        c->deleted++;
        return;
    }
    vfs_error rc = vfs_object_delete(c->store, &o->id);
    if (rc == VFS_OK) { printf("deleted %s\n", o->id.hex); c->deleted++; }
    else {
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
            fprintf(stderr, "viewfs: object delete --orphaned takes no id\n");
            return 2;
        }
        struct orphan_delete_ctx c = { .store = s, .dry_run = dry_run != NULL };
        vfs_error rc = vfs_object_list_orphans(s, orphan_delete_cb, &c);
        if (rc != VFS_OK) return cli_perror(s, rc, "object delete --orphaned");
        printf("%s %d orphan(s)%s\n", c.dry_run ? "would delete" : "deleted",
               c.deleted, c.failed ? " (with failures)" : "");
        return c.failed ? 1 : 0;
    }
    if (dry_run) {
        fprintf(stderr, "viewfs: --dry-run only applies with --orphaned\n");
        return 2;
    }
    if (argc != 4) return usage();
    vfs_object_id id;
    if (cli_resolve_object(s, argv[3], &id)) return 1;
    vfs_error rc = vfs_object_delete(s, &id);
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
    else if (!strcmp(sub, "name"))   rc = sub_name  (argc, argv, s);
    else if (!strcmp(sub, "copy"))   rc = sub_copy  (argc, argv, s);
    else if (!strcmp(sub, "id"))     rc = sub_id    (argc, argv, s);
    else if (!strcmp(sub, "list"))   rc = sub_list  (argc, argv, s);
    else if (!strcmp(sub, "delete")) rc = sub_delete(argc, argv, s);
    else { vfs_store_close(s); return usage(); }

    vfs_store_close(s);
    return rc;
}
