/* Computed membership: the files visible in a directory are the objects
 * whose property set is a superset of the directory's effective property
 * set. Implemented as one EXISTS clause per required (key,value) pair, all
 * AND-ed together. An empty effective set matches every object. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static const char MEMBER_FIELDS[] =
    "o.object_id, o.kind, o.name, o.size, o.mode, o.ctime_ns, o.mtime_ns, "
    "o.atime_ns, o.source_path, o.checksum, o.uid, o.gid, "
    "(o.checksum_state IS NOT NULL)";

static void emit_row(PGresult *r, int i, vfs_object_cb cb, void *ud) {
    vfs_object_info info;
    memset(&info, 0, sizeof info);
    static char id_buf[VFS_OID_HEX_LEN + 1];
    static char kind_buf[16];
    static char name_buf[VFS_NAME_MAX + 1];
    static char src_buf[VFS_PATH_MAX];
    static char chk_buf[128];

    snprintf(id_buf,   sizeof id_buf,   "%s", PQgetvalue(r, i, 0));
    snprintf(kind_buf, sizeof kind_buf, "%s", PQgetvalue(r, i, 1));
    snprintf(name_buf, sizeof name_buf, "%s", PQgetvalue(r, i, 2));
    memcpy(info.id.hex, id_buf, sizeof info.id.hex);
    info.kind     = kind_buf;
    info.name     = name_buf;
    info.size     = atoll(PQgetvalue(r, i, 3));
    info.mode     = atoi(PQgetvalue(r, i, 4));
    info.ctime_ns = atoll(PQgetvalue(r, i, 5));
    info.mtime_ns = atoll(PQgetvalue(r, i, 6));
    info.atime_ns = atoll(PQgetvalue(r, i, 7));
    if (!PQgetisnull(r, i, 8)) {
        snprintf(src_buf, sizeof src_buf, "%s", PQgetvalue(r, i, 8));
        info.source_path = src_buf;
    }
    if (!PQgetisnull(r, i, 9)) {
        snprintf(chk_buf, sizeof chk_buf, "%s", PQgetvalue(r, i, 9));
        info.checksum = chk_buf;
    }
    info.uid = PQgetisnull(r, i, 10) ? -1 : atoi(PQgetvalue(r, i, 10));
    info.gid = PQgetisnull(r, i, 11) ? -1 : atoi(PQgetvalue(r, i, 11));
    info.has_checksum_state =
        !PQgetisnull(r, i, 12) && PQgetvalue(r, i, 12)[0] == 't';
    cb(&info, ud);
}

/* Run a query selecting objects that satisfy every (keys[i], vals[i]) pair
 * (vals[i]==NULL means "any value of keys[i]"), plus an optional name
 * filter. When count_out != NULL, returns count and does not emit. */
static vfs_error emit_matching(vfs_store *s, int npairs,
                               const char *const *keys, const char *const *vals,
                               const char *name_filter,
                               vfs_object_cb cb, void *ud, int64_t *count_out) {
    /* Worst case: 2 params per pair + 1 for the name filter. */
    int cap = 2 * npairs + 1;
    const char **params = cap ? malloc((size_t)cap * sizeof *params) : NULL;
    if (cap && !params) return VFS_ERR_NOMEM;
    /* SQL grows ~150 chars per EXISTS clause. */
    size_t sqlcap = 512 + (size_t)npairs * 160;
    char *sql = malloc(sqlcap);
    if (!sql) { free(params); return VFS_ERR_NOMEM; }

    int np = 0;
    int off = snprintf(sql, sqlcap, "SELECT %s FROM objects o WHERE 1=1",
                       count_out ? "count(*)" : MEMBER_FIELDS);

    for (int i = 0; i < npairs; i++) {
        if (vals[i]) {
            off += snprintf(sql + off, sqlcap - (size_t)off,
                " AND EXISTS (SELECT 1 FROM object_props p "
                "WHERE p.object_id=o.object_id AND p.key=$%d AND p.value=$%d)",
                np + 1, np + 2);
            params[np++] = keys[i];
            params[np++] = vals[i];
        } else {
            off += snprintf(sql + off, sqlcap - (size_t)off,
                " AND EXISTS (SELECT 1 FROM object_props p "
                "WHERE p.object_id=o.object_id AND p.key=$%d)",
                np + 1);
            params[np++] = keys[i];
        }
    }
    if (name_filter) {
        off += snprintf(sql + off, sqlcap - (size_t)off,
                        " AND o.name = $%d", np + 1);
        params[np++] = name_filter;
    }
    if (!count_out)
        snprintf(sql + off, sqlcap - (size_t)off,
                 " ORDER BY o.name, o.object_id");

    PGresult *r = PQexecParams(s->pg, sql, np, NULL, params, NULL, NULL, 0);
    free(sql);
    free(params);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    if (count_out) {
        *count_out = atoll(PQgetvalue(r, 0, 0));
    } else {
        int n = PQntuples(r);
        for (int i = 0; i < n; i++) emit_row(r, i, cb, ud);
    }
    PQclear(r);
    return VFS_OK;
}

/* Collect the directory's effective (key,value) pairs into caller-owned
 * arrays of pointers into `eff` (valid until eff is PQclear'd). */
static vfs_error run_members(vfs_store *s, const char *view, const char *dir_path,
                             const char *name_filter,
                             vfs_object_cb cb, void *ud, int64_t *count_out) {
    int exists = 0;
    vfs_error rc = vfs_dir_exists(s, view, dir_path, &exists);
    if (rc != VFS_OK) return rc;
    if (!exists) return VFS_ERR_NOTFOUND;

    PGresult *eff = vfs_dir_effective_result(s, view, dir_path);
    if (!eff) return VFS_ERR_DB;
    int n = PQntuples(eff);

    const char **keys = NULL, **vals = NULL;
    if (n > 0) {
        keys = malloc((size_t)n * sizeof *keys);
        vals = malloc((size_t)n * sizeof *vals);
        if (!keys || !vals) { free(keys); free(vals); PQclear(eff); return VFS_ERR_NOMEM; }
        for (int i = 0; i < n; i++) {
            keys[i] = PQgetvalue(eff, i, 0);
            vals[i] = PQgetvalue(eff, i, 1);
        }
    }
    rc = emit_matching(s, n, keys, vals, name_filter, cb, ud, count_out);
    free(keys); free(vals);
    PQclear(eff);
    return rc;
}

vfs_error vfs_dir_members(vfs_store *s, const char *view, const char *dir_path,
                          vfs_object_cb cb, void *ud) {
    if (!s || !view || !dir_path || !cb) return VFS_ERR_BADARGS;
    return run_members(s, view, dir_path, NULL, cb, ud, NULL);
}

vfs_error vfs_dir_member_count(vfs_store *s, const char *view,
                               const char *dir_path, int64_t *out) {
    if (!s || !view || !dir_path || !out) return VFS_ERR_BADARGS;
    return run_members(s, view, dir_path, NULL, NULL, NULL, out);
}

vfs_error vfs_dir_members_named(vfs_store *s, const char *view,
                                const char *dir_path, const char *name,
                                vfs_object_cb cb, void *ud) {
    if (!s || !view || !dir_path || !name || !cb) return VFS_ERR_BADARGS;
    return run_members(s, view, dir_path, name, cb, ud, NULL);
}

/* Collector for member_by_name: capture the first match and a count. */
struct name_collect {
    vfs_object_id id;
    int           count;
};
static void name_cb(const vfs_object_info *info, void *ud) {
    struct name_collect *c = ud;
    if (c->count == 0) c->id = info->id;
    c->count++;
}

vfs_error vfs_dir_member_by_name(vfs_store *s, const char *view,
                                 const char *dir_path, const char *name,
                                 vfs_object_id *out) {
    if (!s || !view || !dir_path || !name || !out) return VFS_ERR_BADARGS;
    struct name_collect c = { .count = 0 };
    vfs_error rc = run_members(s, view, dir_path, name, name_cb, &c, NULL);
    if (rc != VFS_OK) return rc;
    if (c.count == 0) return VFS_ERR_NOTFOUND;
    if (c.count > 1)  return VFS_ERR_AMBIGUOUS;
    *out = c.id;
    return VFS_OK;
}
