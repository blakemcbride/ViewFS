/* `vfs prop` -- manage properties on either a FILE (object) or a
 * DIRECTORY (its membership filter). One command covers both because both
 * are just (key,value) pairs; the TARGET decides which.
 *
 * Targets come last (like chmod), so each command takes a list of them:
 *   prop set    KEY=VALUE   [TARGET...] [--flow]
 *   prop unset  KEY[=VALUE] [TARGET...]
 *   prop list   [TARGET...] [--effective]
 * A property is written KEY=VALUE (one token); for unset the value is
 * optional and a bare KEY removes every value of the key. The operation is
 * applied to every TARGET; with none, the directory you are in (inside a
 * mounted view) is used.
 *
 * TARGET resolution:
 *   "."  / a path / name  resolved against your current directory; a
 *                         directory -> its filter, a file -> its properties
 *   VIEW:DIR              a directory in any view, e.g. docs:/reports
 *   ID|PREFIX             an object (file) by id
 *
 * --flow (set) and --effective (list) apply only to directory targets. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

enum tgt_kind { TGT_DIR, TGT_OBJECT };
struct prop_target {
    enum tgt_kind kind;
    char          view[128];
    char          dir[VFS_PATH_MAX];
    vfs_object_id id;
};

static void print_usage(FILE *out) {
    fprintf(out,
"Usage: vfs prop <subcommand>\n"
"  prop set    KEY=VALUE   [TARGET...] [--flow]\n"
"  prop unset  KEY[=VALUE] [TARGET...]\n"
"  prop list   [TARGET...] [--effective]\n"
"\n"
"  The operation applies to every TARGET; omit them to use the directory you\n"
"  are in (inside a mounted view). A TARGET is one of:\n"
"    .  / path / name  resolved against your cwd; dir -> filter, file -> props\n"
"    VIEW:DIR          a directory in any view (e.g. docs:/reports)\n"
"    ID|PREFIX         an object (file) by id\n"
"  A property is written KEY=VALUE; for unset the value is optional and a\n"
"  bare KEY removes every value of that key.\n"
"  --flow / --effective apply to directory targets.\n");
}
static int usage(void) { print_usage(stderr); return 2; }

static void print_oprop(const vfs_prop_row *p, void *ud) {
    (void)ud;
    printf("  %-24s %s\n", p->key, p->value);
}
static void print_dprop(const vfs_dirprop_row *p, void *ud) {
    int *effective = ud;
    if (*effective)
        printf("  %-20s %-20s %s  [from %s]\n", p->key, p->value,
               p->flow ? "flow" : "    ", p->source_dir);
    else
        printf("  %-20s %-20s %s\n", p->key, p->value, p->flow ? "flow" : "");
}

/* Fill `label` (size lsz) with a human-readable name for the target. */
static void target_label(const struct prop_target *t, char *label, size_t lsz) {
    if (t->kind == TGT_DIR) snprintf(label, lsz, "%s:%s", t->view, t->dir);
    else                    snprintf(label, lsz, "%s", t->id.hex);
}

/* Resolve TARGET (tok==NULL means the current directory) into *t. Prints a
 * message and returns -1 on failure. */
static int resolve_target(vfs_store *s, const char *tok, struct prop_target *t) {
    if (!tok) {
        if (cli_resolve_dir(0, NULL, NULL, t->view, sizeof t->view,
                            t->dir, sizeof t->dir) != 0) return -1;
        t->kind = TGT_DIR;
        return 0;
    }

    /* VIEW:DIR -> an explicit directory in any view. */
    const char *colon = strchr(tok, ':');
    if (colon && colon != tok && colon[1]) {
        char vpart[128];
        size_t vl = (size_t)(colon - tok);
        if (vl >= sizeof vpart) { fprintf(stderr, "vfs: view name too long\n"); return -1; }
        memcpy(vpart, tok, vl); vpart[vl] = '\0';
        if (cli_resolve_dir(2, vpart, colon + 1, t->view, sizeof t->view,
                            t->dir, sizeof t->dir) != 0) return -1;
        t->kind = TGT_DIR;
        return 0;
    }

    /* Inside a mount: resolve tok as a path (relative to cwd or absolute),
     * then classify it as a directory or a file. */
    char cv[128], cd[VFS_PATH_MAX];
    if (cli_view_dir_from_cwd(cv, sizeof cv, cd, sizeof cd) == 0) {
        char rv[128], rp[VFS_PATH_MAX];
        if (cli_resolve_dir(1, tok, NULL, rv, sizeof rv, rp, sizeof rp) == 0) {
            int isdir = 0;
            if (vfs_dir_exists(s, rv, rp, &isdir) == VFS_OK && isdir) {
                t->kind = TGT_DIR;
                snprintf(t->view, sizeof t->view, "%s", rv);
                snprintf(t->dir,  sizeof t->dir,  "%s", rp);
                return 0;
            }
            vfs_canon_path cp;
            if (vfs_path_canonicalize(rp, &cp) == VFS_OK && !cp.is_root) {
                const char *parent = cp.parent[0] ? cp.parent : "/";
                vfs_error e = vfs_dir_member_by_name(s, rv, parent, cp.name, &t->id);
                if (e == VFS_OK)            { t->kind = TGT_OBJECT; return 0; }
                if (e == VFS_ERR_AMBIGUOUS) {
                    fprintf(stderr, "vfs: '%s' names more than one object\n", tok);
                    return -1;
                }
            }
        }
    }

    /* Otherwise interpret tok as an object id / unique prefix. */
    if (vfs_object_resolve(s, tok, &t->id) == VFS_OK) { t->kind = TGT_OBJECT; return 0; }

    fprintf(stderr, "vfs: cannot resolve '%s' to a directory or object\n", tok);
    return -1;
}

int cmd_prop(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *sub = argv[2];
    if (cli_is_help_request(sub)) { print_usage(stdout); return 0; }

    const char *flow_flag = cli_take_flag(&argc, argv, "--flow", 1);
    const char *eff_flag  = cli_take_flag(&argc, argv, "--effective", 1);

    int rc_open = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc_open);
    if (!s) return rc_open;

    char label[128 + VFS_PATH_MAX];
    struct prop_target t;
    int rc;

    if (!strcmp(sub, "set")) {
        /* KEY=VALUE [TARGET...]: set the property on each TARGET (or the cwd
         * directory when none are given). */
        if (argc < 4) { rc = usage(); goto done; }
        char *spec = strdup(argv[3]);
        char *eq = spec ? strchr(spec, '=') : NULL;
        if (!eq) {
            fprintf(stderr, "vfs: prop set needs KEY=VALUE\n");
            free(spec); rc = usage(); goto done;
        }
        *eq = '\0';
        const char *key = spec, *val = eq + 1;
        int ntgt = argc - 4, count = ntgt > 0 ? ntgt : 1;
        rc = 0;
        for (int i = 0; i < count; i++) {
            const char *tok = ntgt > 0 ? argv[4 + i] : NULL;
            if (resolve_target(s, tok, &t)) { rc = 1; continue; }
            target_label(&t, label, sizeof label);
            vfs_error e;
            if (t.kind == TGT_DIR) {
                e = vfs_dir_prop_set(s, t.view, t.dir, key, val, flow_flag != NULL);
            } else if (flow_flag) {
                fprintf(stderr, "vfs: --flow applies only to directories\n");
                rc = 2; continue;
            } else {
                e = vfs_object_prop_add(s, &t.id, key, val);
            }
            if (e != VFS_OK) rc = cli_perror(s, e, "prop set");
            else printf("%s  %s=%s%s\n", label, key, val,
                        (t.kind == TGT_DIR && flow_flag) ? " (flow)" : "");
        }
        free(spec);
    } else if (!strcmp(sub, "unset")) {
        /* KEY[=VALUE] [TARGET...]: remove the property from each TARGET (or the
         * cwd directory). A bare KEY removes every value of that key. */
        if (argc < 4) { rc = usage(); goto done; }
        char *spec = strdup(argv[3]);
        char *eq = spec ? strchr(spec, '=') : NULL;
        const char *val = NULL;
        if (eq) { *eq = '\0'; val = eq + 1; }
        const char *key = spec;
        int ntgt = argc - 4, count = ntgt > 0 ? ntgt : 1;
        rc = 0;
        for (int i = 0; i < count; i++) {
            const char *tok = ntgt > 0 ? argv[4 + i] : NULL;
            if (resolve_target(s, tok, &t)) { rc = 1; continue; }
            target_label(&t, label, sizeof label);
            vfs_error e = (t.kind == TGT_DIR)
                ? vfs_dir_prop_delete(s, t.view, t.dir, key, val)
                : vfs_object_prop_unset(s, &t.id, key, val);
            if (e == VFS_ERR_NOTFOUND) {
                fprintf(stderr, "vfs: %s has no matching property\n", label);
                rc = 1;
            } else if (e != VFS_OK) {
                rc = cli_perror(s, e, "prop unset");
            } else {
                printf("Unset %s %s%s%s\n", label, key, val ? "=" : "", val ? val : "");
            }
        }
        free(spec);
    } else if (!strcmp(sub, "list")) {
        /* [TARGET...]: list properties of each TARGET (or the cwd directory). */
        int ntgt = argc - 3, count = ntgt > 0 ? ntgt : 1;
        rc = 0;
        for (int i = 0; i < count; i++) {
            const char *tok = ntgt > 0 ? argv[3 + i] : NULL;
            if (resolve_target(s, tok, &t)) { rc = 1; continue; }
            if (t.kind == TGT_DIR) {
                int exists = 0;
                vfs_dir_exists(s, t.view, t.dir, &exists);
                if (!exists) {
                    fprintf(stderr, "vfs: no directory %s:%s\n", t.view, t.dir);
                    rc = 1; continue;
                }
                int effective = eff_flag != NULL;
                printf("%s:%s %s filter:\n", t.view, t.dir, effective ? "effective" : "own");
                vfs_error e = vfs_dir_prop_list(s, t.view, t.dir, effective,
                                                print_dprop, &effective);
                if (e != VFS_OK) rc = cli_perror(s, e, "prop list");
            } else {
                if (eff_flag) {
                    fprintf(stderr, "vfs: --effective applies only to directories\n");
                    rc = 2; continue;
                }
                printf("properties of %s:\n", t.id.hex);
                vfs_error e = vfs_object_prop_list(s, &t.id, print_oprop, NULL);
                if (e != VFS_OK) rc = cli_perror(s, e, "prop list");
            }
        }
    } else {
        rc = usage();
    }
done:
    vfs_store_close(s);
    return rc;
}
