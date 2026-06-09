/* The per-view directory tree and the property filters attached to each
 * directory. Replaces the old explicit `mappings` table. Files are never
 * stored here; a directory's file contents are computed by members.c from
 * the directory's effective property set. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* Stored parent_path convention:
 *   root           -> dir_path "/",  parent_path ""   (root has no parent)
 *   child of root  -> dir_path "/a", parent_path "/"
 *   deeper         -> dir_path "/a/b", parent_path "/a"
 * The canonicalizer yields parent="" for a direct child of root, so we
 * translate "" -> "/" for non-root rows. */
static const char *stored_parent(const vfs_canon_path *cp) {
    if (cp->is_root) return "";
    return cp->parent[0] ? cp->parent : "/";
}

/* Insert one directory row. conflict_ok=1 -> ON CONFLICT DO NOTHING. */
static vfs_error insert_dir(vfs_store *s, const char *view, const char *dir_path,
                            const char *parent, const char *name, int mode,
                            int conflict_ok) {
    int64_t now = vfs_now_ns();
    char now_s[24], mode_s[16];
    snprintf(now_s,  sizeof now_s,  "%lld", (long long)now);
    snprintf(mode_s, sizeof mode_s, "%d",   mode);
    const char *params[6] = { view, dir_path, parent, name, mode_s, now_s };
    const char *sql = conflict_ok
        ? "INSERT INTO view_dirs "
          "(view_name, dir_path, parent_path, name, mode, ctime_ns, mtime_ns) "
          "VALUES ($1,$2,$3,$4,$5::int,$6::bigint,$6::bigint) "
          "ON CONFLICT (view_name, dir_path) DO NOTHING"
        : "INSERT INTO view_dirs "
          "(view_name, dir_path, parent_path, name, mode, ctime_ns, mtime_ns) "
          "VALUES ($1,$2,$3,$4,$5::int,$6::bigint,$6::bigint)";
    PGresult *r = PQexecParams(s->pg, sql, 6, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        vfs_seterr_pq(s, r); PQclear(r);
        if (code && !strcmp(code, "23505")) return VFS_ERR_EXISTS;
        if (code && !strcmp(code, "23503")) return VFS_ERR_NOTFOUND; /* fk: view */
        return VFS_ERR_DB;
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_dir_ensure_root(vfs_store *s, const char *view) {
    if (!s || !view) return VFS_ERR_BADARGS;
    return insert_dir(s, view, "/", "", "", 0755, 1);
}

/* Create directory rows for every ancestor of cp that does not yet exist,
 * including the root. For "/a/b/c", ensures "/", "/a", "/a/b". */
static vfs_error ensure_ancestors(vfs_store *s, const char *view,
                                  const vfs_canon_path *cp) {
    vfs_error rc = vfs_dir_ensure_root(s, view);
    if (rc != VFS_OK) return rc;
    if (cp->is_root) return VFS_OK;

    const char *p = cp->path + 1;            /* skip leading '/' */
    char accum[VFS_PATH_MAX];
    size_t accum_len = 0;
    const char *parent = "/";
    for (;;) {
        const char *slash = strchr(p, '/');
        if (!slash) break;                   /* p is the final component */
        size_t seglen = (size_t)(slash - p);
        char parent_buf[VFS_PATH_MAX];
        snprintf(parent_buf, sizeof parent_buf, "%s", accum_len ? accum : "/");
        if (accum_len + 1 + seglen >= sizeof accum) return VFS_ERR_INTERNAL;
        accum[accum_len++] = '/';
        memcpy(accum + accum_len, p, seglen);
        accum_len += seglen;
        accum[accum_len] = '\0';
        char name_buf[VFS_NAME_MAX + 1];
        snprintf(name_buf, sizeof name_buf, "%.*s", (int)seglen, p);
        rc = insert_dir(s, view, accum, parent_buf, name_buf, 0755, 1);
        if (rc != VFS_OK) return rc;
        parent = accum;
        p = slash + 1;
    }
    (void)parent;
    return VFS_OK;
}

vfs_error vfs_dir_create(vfs_store *s, const char *view, const char *dir_path,
                         int mode) {
    if (!s || !view || !dir_path) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;
    if (cp.is_root) return VFS_ERR_EXISTS;   /* root always present */

    rc = vfs_exec_simple(s, "BEGIN");
    if (rc != VFS_OK) return rc;
    rc = ensure_ancestors(s, view, &cp);
    if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }
    rc = insert_dir(s, view, cp.path, stored_parent(&cp), cp.name, mode, 0);
    if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }
    rc = vfs_exec_simple(s, "COMMIT");
    if (rc == VFS_OK) {
        vfs_notify_path(s, view, stored_parent(&cp));
        vfs_notify_path(s, view, "");
    }
    return rc;
}

vfs_error vfs_dir_exists(vfs_store *s, const char *view, const char *dir_path,
                         int *out) {
    if (!s || !view || !dir_path || !out) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;
    const char *params[2] = { view, cp.path };
    PGresult *r = PQexecParams(s->pg,
        "SELECT 1 FROM view_dirs WHERE view_name = $1 AND dir_path = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    *out = PQntuples(r) > 0;
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_dir_remove(vfs_store *s, const char *view, const char *dir_path) {
    if (!s || !view || !dir_path) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;
    if (cp.is_root) return VFS_ERR_BADARGS;  /* cannot remove root */

    int exists = 0;
    rc = vfs_dir_exists(s, view, cp.path, &exists);
    if (rc != VFS_OK) return rc;
    if (!exists) return VFS_ERR_NOTFOUND;

    /* Refuse if it has child directories. */
    const char *params[2] = { view, cp.path };
    PGresult *r = PQexecParams(s->pg,
        "SELECT count(*) FROM view_dirs WHERE view_name = $1 AND parent_path = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int children = atoi(PQgetvalue(r, 0, 0));
    PQclear(r);
    if (children > 0) return VFS_ERR_NOTEMPTY;

    /* Refuse if any file currently matches (non-empty membership). */
    int64_t members = 0;
    rc = vfs_dir_member_count(s, view, cp.path, &members);
    if (rc != VFS_OK) return rc;
    if (members > 0) return VFS_ERR_NOTEMPTY;

    r = PQexecParams(s->pg,
        "DELETE FROM view_dirs WHERE view_name = $1 AND dir_path = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    if (affected) {
        vfs_notify_path(s, view, stored_parent(&cp));
        vfs_notify_path(s, view, "");
    }
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
}

vfs_error vfs_dir_get(vfs_store *s, const char *view, const char *dir_path,
                      vfs_dir_row *out) {
    if (!s || !view || !dir_path || !out) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;
    static char view_b[256], path_b[VFS_PATH_MAX], parent_b[VFS_PATH_MAX],
                name_b[VFS_NAME_MAX + 1];
    const char *params[2] = { view, cp.path };
    PGresult *r = PQexecParams(s->pg,
        "SELECT dir_path, parent_path, name, mode, ctime_ns, mtime_ns "
        "FROM view_dirs WHERE view_name = $1 AND dir_path = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    if (PQntuples(r) == 0) { PQclear(r); return VFS_ERR_NOTFOUND; }
    snprintf(view_b,   sizeof view_b,   "%s", view);
    snprintf(path_b,   sizeof path_b,   "%s", PQgetvalue(r, 0, 0));
    snprintf(parent_b, sizeof parent_b, "%s", PQgetvalue(r, 0, 1));
    snprintf(name_b,   sizeof name_b,   "%s", PQgetvalue(r, 0, 2));
    out->view_name   = view_b;
    out->dir_path    = path_b;
    out->parent_path = parent_b;
    out->name        = name_b;
    out->mode        = atoi(PQgetvalue(r, 0, 3));
    out->ctime_ns    = atoll(PQgetvalue(r, 0, 4));
    out->mtime_ns    = atoll(PQgetvalue(r, 0, 5));
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_dir_list_children(vfs_store *s, const char *view,
                                const char *dir_path, vfs_dir_cb cb, void *ud) {
    if (!s || !view || !dir_path || !cb) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;
    /* Children carry parent_path == this directory's dir_path. */
    const char *params[2] = { view, cp.path };
    PGresult *r = PQexecParams(s->pg,
        "SELECT dir_path, parent_path, name, mode, ctime_ns, mtime_ns "
        "FROM view_dirs WHERE view_name = $1 AND parent_path = $2 "
        "ORDER BY name",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        vfs_dir_row row;
        row.view_name   = view;
        row.dir_path    = PQgetvalue(r, i, 0);
        row.parent_path = PQgetvalue(r, i, 1);
        row.name        = PQgetvalue(r, i, 2);
        row.mode        = atoi(PQgetvalue(r, i, 3));
        row.ctime_ns    = atoll(PQgetvalue(r, i, 4));
        row.mtime_ns    = atoll(PQgetvalue(r, i, 5));
        cb(&row, ud);
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_dir_set_mode(vfs_store *s, const char *view, const char *dir_path,
                           int mode) {
    if (!s || !view || !dir_path) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;
    char m[16]; snprintf(m, sizeof m, "%d", mode);
    const char *p[3] = { m, view, cp.path };
    PGresult *r = PQexecParams(s->pg,
        "UPDATE view_dirs SET mode = $1::int "
        "WHERE view_name = $2 AND dir_path = $3",
        3, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
}

vfs_error vfs_dir_rename(vfs_store *s, const char *view,
                         const char *from, const char *to) {
    if (!s || !view || !from || !to) return VFS_ERR_BADARGS;
    vfs_canon_path sf, df;
    vfs_error rc = vfs_path_canonicalize(from, &sf);
    if (rc != VFS_OK) return rc;
    rc = vfs_path_canonicalize(to, &df);
    if (rc != VFS_OK) return rc;
    if (sf.is_root || df.is_root) return VFS_ERR_BADARGS;
    if (!strcmp(sf.path, df.path)) return VFS_OK;
    /* reject move into own descendant */
    size_t slen = strlen(sf.path);
    if (!strncmp(df.path, sf.path, slen) && df.path[slen] == '/')
        return VFS_ERR_BADARGS;

    int exists = 0;
    rc = vfs_dir_exists(s, view, sf.path, &exists);
    if (rc != VFS_OK) return rc;
    if (!exists) return VFS_ERR_NOTFOUND;
    rc = vfs_dir_exists(s, view, df.path, &exists);
    if (rc != VFS_OK) return rc;
    if (exists) return VFS_ERR_EXISTS;

    rc = vfs_exec_simple(s, "BEGIN");
    if (rc != VFS_OK) return rc;
    rc = ensure_ancestors(s, view, &df);
    if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }

    char off_s[16];
    snprintf(off_s, sizeof off_s, "%d", (int)slen + 1);
    char like[VFS_PATH_MAX];
    snprintf(like, sizeof like, "%s/%%", sf.path);

    /* Step 1: rewrite descendants (view_dir_props follow via ON UPDATE CASCADE). */
    const char *p1[5] = { view, sf.path, df.path, off_s, like };
    PGresult *r = PQexecParams(s->pg,
        "UPDATE view_dirs SET "
        "  dir_path    = $3 || substr(dir_path,    $4::int), "
        "  parent_path = $3 || substr(parent_path, $4::int) "
        "WHERE view_name = $1 AND dir_path LIKE $5",
        5, NULL, p1, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); vfs_exec_simple(s, "ROLLBACK");
        return VFS_ERR_DB;
    }
    PQclear(r);

    /* Step 2: move the directory row itself. */
    const char *new_parent = df.parent[0] ? df.parent : "/";
    const char *p2[5] = { view, df.path, new_parent, df.name, sf.path };
    r = PQexecParams(s->pg,
        "UPDATE view_dirs SET dir_path = $2, parent_path = $3, name = $4 "
        "WHERE view_name = $1 AND dir_path = $5",
        5, NULL, p2, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); vfs_exec_simple(s, "ROLLBACK");
        return VFS_ERR_DB;
    }
    PQclear(r);

    rc = vfs_exec_simple(s, "COMMIT");
    if (rc == VFS_OK) {
        vfs_notify_path(s, view, "");
    }
    return rc;
}

/* ----------------------------------------------------------------
 * Directory property pairs
 * ---------------------------------------------------------------- */

static vfs_error dir_prop_upsert(vfs_store *s, const char *view,
                                 const char *dir_path, const char *key,
                                 const char *value, int flow, int update_flow) {
    if (!s || !view || !dir_path || !key || !*key || !value) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;
    int64_t now = vfs_now_ns();
    char now_s[24];
    snprintf(now_s, sizeof now_s, "%lld", (long long)now);
    const char *flow_s = flow ? "t" : "f";
    const char *params[6] = { view, cp.path, key, value, flow_s, now_s };
    const char *sql = update_flow
        ? "INSERT INTO view_dir_props "
          "(view_name, dir_path, key, value, flow, ctime_ns, mtime_ns) "
          "VALUES ($1,$2,$3,$4,$5::bool,$6::bigint,$6::bigint) "
          "ON CONFLICT (view_name, dir_path, key, value) DO UPDATE "
          "  SET flow = EXCLUDED.flow, mtime_ns = EXCLUDED.mtime_ns"
        : "INSERT INTO view_dir_props "
          "(view_name, dir_path, key, value, flow, ctime_ns, mtime_ns) "
          "VALUES ($1,$2,$3,$4,$5::bool,$6::bigint,$6::bigint) "
          "ON CONFLICT (view_name, dir_path, key, value) DO NOTHING";
    PGresult *r = PQexecParams(s->pg, sql, 6, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        vfs_seterr_pq(s, r); PQclear(r);
        if (code && !strcmp(code, "23503")) return VFS_ERR_NOTFOUND; /* fk: dir */
        return VFS_ERR_DB;
    }
    PQclear(r);
    vfs_notify_path(s, view, "");   /* membership is view-global */
    return VFS_OK;
}

vfs_error vfs_dir_prop_add(vfs_store *s, const char *view, const char *dir_path,
                           const char *key, const char *value, int flow) {
    return dir_prop_upsert(s, view, dir_path, key, value, flow, 0);
}

vfs_error vfs_dir_prop_set(vfs_store *s, const char *view, const char *dir_path,
                           const char *key, const char *value, int flow) {
    return dir_prop_upsert(s, view, dir_path, key, value, flow, 1);
}

vfs_error vfs_dir_prop_delete(vfs_store *s, const char *view, const char *dir_path,
                              const char *key, const char *value) {
    if (!s || !view || !dir_path || !key) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;
    PGresult *r;
    if (value) {
        const char *params[4] = { view, cp.path, key, value };
        r = PQexecParams(s->pg,
            "DELETE FROM view_dir_props "
            "WHERE view_name=$1 AND dir_path=$2 AND key=$3 AND value=$4",
            4, NULL, params, NULL, NULL, 0);
    } else {
        const char *params[3] = { view, cp.path, key };
        r = PQexecParams(s->pg,
            "DELETE FROM view_dir_props "
            "WHERE view_name=$1 AND dir_path=$2 AND key=$3",
            3, NULL, params, NULL, NULL, 0);
    }
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    if (affected) vfs_notify_path(s, view, "");
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
}

/* Build a list of ancestor dir_paths (excluding the directory itself),
 * including "/". Returns the count, or -1 on overflow. anc[] holds copies. */
static int compute_ancestors(const char *dir_path,
                             char anc[][VFS_PATH_MAX], int max) {
    int n = 0;
    if (!strcmp(dir_path, "/")) return 0;
    if (n >= max) return -1;
    snprintf(anc[n++], VFS_PATH_MAX, "/");      /* root */
    /* intermediate prefixes: every '/' after position 0 ends an ancestor */
    for (size_t i = 1; dir_path[i]; i++) {
        if (dir_path[i] == '/') {
            if (n >= max) return -1;
            snprintf(anc[n], VFS_PATH_MAX, "%.*s", (int)i, dir_path);
            n++;
        }
    }
    return n;
}

#define VFS_MAX_ANCESTORS 256

PGresult *vfs_dir_effective_result(vfs_store *s, const char *view,
                                   const char *dir_path) {
    vfs_canon_path cp;
    if (vfs_path_canonicalize(dir_path, &cp) != VFS_OK) {
        vfs_seterr(s, "invalid directory path");
        return NULL;
    }
    static char anc[VFS_MAX_ANCESTORS][VFS_PATH_MAX];
    int nanc = compute_ancestors(cp.path, anc, VFS_MAX_ANCESTORS);
    if (nanc < 0) { vfs_seterr(s, "directory nesting too deep"); return NULL; }

    /* params: $1=view, $2=dir_path, $3.. = ancestors */
    int nparams = 2 + nanc;
    const char **params = malloc((size_t)nparams * sizeof *params);
    if (!params) { vfs_seterr(s, "out of memory"); return NULL; }
    params[0] = view;
    params[1] = cp.path;
    for (int i = 0; i < nanc; i++) params[2 + i] = anc[i];

    char sql[4096];
    int off = snprintf(sql, sizeof sql,
        "SELECT DISTINCT key, value FROM view_dir_props "
        "WHERE view_name = $1 AND (dir_path = $2");
    if (nanc > 0) {
        off += snprintf(sql + off, sizeof sql - (size_t)off,
                        " OR (flow = TRUE AND dir_path IN (");
        for (int i = 0; i < nanc; i++)
            off += snprintf(sql + off, sizeof sql - (size_t)off,
                            "%s$%d", i ? "," : "", 3 + i);
        off += snprintf(sql + off, sizeof sql - (size_t)off, "))");
    }
    snprintf(sql + off, sizeof sql - (size_t)off, ")");

    PGresult *r = PQexecParams(s->pg, sql, nparams, NULL, params, NULL, NULL, 0);
    free(params);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return NULL;
    }
    return r;
}

vfs_error vfs_dir_prop_list(vfs_store *s, const char *view, const char *dir_path,
                            int effective, vfs_dirprop_cb cb, void *ud) {
    if (!s || !view || !dir_path || !cb) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(dir_path, &cp);
    if (rc != VFS_OK) return rc;

    if (!effective) {
        const char *params[2] = { view, cp.path };
        PGresult *r = PQexecParams(s->pg,
            "SELECT key, value, flow, dir_path, ctime_ns, mtime_ns "
            "FROM view_dir_props WHERE view_name=$1 AND dir_path=$2 "
            "ORDER BY key, value",
            2, NULL, params, NULL, NULL, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
        }
        int n = PQntuples(r);
        for (int i = 0; i < n; i++) {
            vfs_dirprop_row row;
            row.key        = PQgetvalue(r, i, 0);
            row.value      = PQgetvalue(r, i, 1);
            row.flow       = PQgetvalue(r, i, 2)[0] == 't';
            row.source_dir = PQgetvalue(r, i, 3);
            row.ctime_ns   = atoll(PQgetvalue(r, i, 4));
            row.mtime_ns   = atoll(PQgetvalue(r, i, 5));
            cb(&row, ud);
        }
        PQclear(r);
        return VFS_OK;
    }

    /* Effective listing: own pairs (any flow) plus flowed ancestor pairs. */
    static char anc[VFS_MAX_ANCESTORS][VFS_PATH_MAX];
    int nanc = compute_ancestors(cp.path, anc, VFS_MAX_ANCESTORS);
    if (nanc < 0) { vfs_seterr(s, "directory nesting too deep"); return VFS_ERR_INTERNAL; }
    int nparams = 2 + nanc;
    const char **params = malloc((size_t)nparams * sizeof *params);
    if (!params) return VFS_ERR_NOMEM;
    params[0] = view; params[1] = cp.path;
    for (int i = 0; i < nanc; i++) params[2 + i] = anc[i];

    char sql[4096];
    int off = snprintf(sql, sizeof sql,
        "SELECT key, value, flow, dir_path, ctime_ns, mtime_ns "
        "FROM view_dir_props WHERE view_name=$1 AND (dir_path=$2");
    if (nanc > 0) {
        off += snprintf(sql + off, sizeof sql - (size_t)off,
                        " OR (flow = TRUE AND dir_path IN (");
        for (int i = 0; i < nanc; i++)
            off += snprintf(sql + off, sizeof sql - (size_t)off,
                            "%s$%d", i ? "," : "", 3 + i);
        off += snprintf(sql + off, sizeof sql - (size_t)off, "))");
    }
    snprintf(sql + off, sizeof sql - (size_t)off, ") ORDER BY key, value");

    PGresult *r = PQexecParams(s->pg, sql, nparams, NULL, params, NULL, NULL, 0);
    free(params);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        vfs_dirprop_row row;
        row.key        = PQgetvalue(r, i, 0);
        row.value      = PQgetvalue(r, i, 1);
        row.flow       = PQgetvalue(r, i, 2)[0] == 't';
        row.source_dir = PQgetvalue(r, i, 3);
        row.ctime_ns   = atoll(PQgetvalue(r, i, 4));
        row.mtime_ns   = atoll(PQgetvalue(r, i, 5));
        cb(&row, ud);
    }
    PQclear(r);
    return VFS_OK;
}
