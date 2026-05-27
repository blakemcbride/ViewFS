#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* Auto-create directory mappings for every ancestor of view_path that does
 * not yet exist. Idempotent. Caller is responsible for canonicalization.
 *
 * For path "/a/b/c", this ensures "/a" and "/a/b" exist (NOT "/a/b/c" itself).
 */
static vfs_error ensure_parent_dirs(vfs_store *s, const char *view,
                                    const vfs_canon_path *cp) {
    if (cp->is_root) return VFS_OK;
    /* Walk components from leftmost to rightmost-but-one. */
    const char *p = cp->path + 1;  /* skip leading '/' */
    char accum[VFS_PATH_MAX];
    size_t accum_len = 0;

    for (;;) {
        const char *slash = strchr(p, '/');
        if (!slash) break;  /* p now points at the final component */
        size_t seglen = (size_t)(slash - p);
        if (accum_len + 1 + seglen >= sizeof accum) return VFS_ERR_INTERNAL;
        accum[accum_len++] = '/';
        memcpy(accum + accum_len, p, seglen);
        accum_len += seglen;
        accum[accum_len] = '\0';

        /* parent of accum */
        const char *last_slash = strrchr(accum, '/');
        const char *parent_str = "";
        char parent_buf[VFS_PATH_MAX];
        if (last_slash && last_slash != accum) {
            size_t pl = (size_t)(last_slash - accum);
            memcpy(parent_buf, accum, pl);
            parent_buf[pl] = '\0';
            parent_str = parent_buf;
        }
        const char *name_str = last_slash ? last_slash + 1 : accum;

        int64_t now = vfs_now_ns();
        char now_s[24];
        snprintf(now_s, sizeof now_s, "%lld", (long long)now);

        const char *params[6] = {
            view, accum, parent_str, name_str, now_s, now_s
        };
        PGresult *r = PQexecParams(s->pg,
            "INSERT INTO mappings "
            "(view_name, view_path, parent_path, name, entry_kind, object_id, "
            " ctime_ns, mtime_ns) "
            "VALUES ($1, $2, $3, $4, 'dir', NULL, $5::bigint, $6::bigint) "
            "ON CONFLICT (view_name, view_path) DO NOTHING",
            6, NULL, params, NULL, NULL, 0);
        if (PQresultStatus(r) != PGRES_COMMAND_OK) {
            vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
        }
        PQclear(r);

        p = slash + 1;
    }
    return VFS_OK;
}

vfs_error vfs_mapping_add_file(vfs_store *s, const char *view, const char *view_path,
                               const vfs_object_id *id) {
    if (!s || !view || !view_path || !id) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(view_path, &cp);
    if (rc != VFS_OK) return rc;
    if (cp.is_root) return VFS_ERR_ISDIR;

    rc = vfs_exec_simple(s, "BEGIN");
    if (rc != VFS_OK) return rc;

    rc = ensure_parent_dirs(s, view, &cp);
    if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }

    int64_t now = vfs_now_ns();
    char now_s[24];
    snprintf(now_s, sizeof now_s, "%lld", (long long)now);

    const char *params[7] = {
        view, cp.path, cp.parent, cp.name, id->hex, now_s, now_s
    };
    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO mappings "
        "(view_name, view_path, parent_path, name, entry_kind, object_id, "
        " ctime_ns, mtime_ns) "
        "VALUES ($1, $2, $3, $4, 'file', $5, $6::bigint, $7::bigint)",
        7, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        vfs_seterr_pq(s, r);
        PQclear(r);
        vfs_exec_simple(s, "ROLLBACK");
        if (code && !strcmp(code, "23505")) return VFS_ERR_EXISTS;
        if (code && !strcmp(code, "23503")) return VFS_ERR_NOTFOUND; /* fk */
        return VFS_ERR_DB;
    }
    PQclear(r);

    vfs_error commit_rc = vfs_exec_simple(s, "COMMIT");
    if (commit_rc == VFS_OK) {
        vfs_notify_path(s, view, cp.parent);
        if (cp.parent[0]) vfs_notify_path(s, view, "");
    }
    return commit_rc;
}

vfs_error vfs_mapping_add_dir(vfs_store *s, const char *view, const char *view_path,
                              int mode) {
    if (!s || !view || !view_path) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(view_path, &cp);
    if (rc != VFS_OK) return rc;
    if (cp.is_root) return VFS_OK;  /* root always implicit */

    rc = vfs_exec_simple(s, "BEGIN");
    if (rc != VFS_OK) return rc;

    rc = ensure_parent_dirs(s, view, &cp);
    if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }

    int64_t now = vfs_now_ns();
    char now_s[24], mode_s[16];
    snprintf(now_s,  sizeof now_s,  "%lld", (long long)now);
    snprintf(mode_s, sizeof mode_s, "%d",   mode);

    const char *params[7] = {
        view, cp.path, cp.parent, cp.name, mode_s, now_s, now_s
    };
    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO mappings "
        "(view_name, view_path, parent_path, name, entry_kind, object_id, "
        " mode_override, ctime_ns, mtime_ns) "
        "VALUES ($1, $2, $3, $4, 'dir', NULL, $5::int, $6::bigint, $7::bigint)",
        7, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        vfs_seterr_pq(s, r);
        PQclear(r);
        vfs_exec_simple(s, "ROLLBACK");
        if (code && !strcmp(code, "23505")) return VFS_ERR_EXISTS;
        return VFS_ERR_DB;
    }
    PQclear(r);

    vfs_error commit_rc = vfs_exec_simple(s, "COMMIT");
    if (commit_rc == VFS_OK) {
        vfs_notify_path(s, view, cp.parent);
        if (cp.parent[0]) vfs_notify_path(s, view, "");
    }
    return commit_rc;
}

vfs_error vfs_mapping_remove(vfs_store *s, const char *view, const char *view_path) {
    if (!s || !view || !view_path) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(view_path, &cp);
    if (rc != VFS_OK) return rc;
    if (cp.is_root) return VFS_ERR_ISDIR;

    /* Check whether it's a non-empty directory. */
    const char *params2[2] = { view, cp.path };
    PGresult *r = PQexecParams(s->pg,
        "SELECT entry_kind, "
        "       (SELECT count(*) FROM mappings c "
        "        WHERE c.view_name = $1 AND c.parent_path = $2) AS children "
        "FROM mappings WHERE view_name = $1 AND view_path = $2",
        2, NULL, params2, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    if (PQntuples(r) == 0) { PQclear(r); return VFS_ERR_NOTFOUND; }
    const char *kind = PQgetvalue(r, 0, 0);
    int children = atoi(PQgetvalue(r, 0, 1));
    if (!strcmp(kind, "dir") && children > 0) {
        PQclear(r);
        return VFS_ERR_NOTEMPTY;
    }
    PQclear(r);

    r = PQexecParams(s->pg,
        "DELETE FROM mappings WHERE view_name = $1 AND view_path = $2",
        2, NULL, params2, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    if (affected) {
        vfs_notify_path(s, view, cp.parent);
        if (cp.parent[0]) vfs_notify_path(s, view, "");
    }
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
}

static const char MAPPING_SELECT[] =
    "SELECT view_name, view_path, parent_path, name, entry_kind, "
    "       COALESCE(object_id, ''), ctime_ns, mtime_ns FROM mappings ";

static void fill_mapping_row(vfs_mapping_row *row, PGresult *r, int i,
                             char id_buf[VFS_OID_HEX_LEN + 1]) {
    row->view_name   = PQgetvalue(r, i, 0);
    row->view_path   = PQgetvalue(r, i, 1);
    row->parent_path = PQgetvalue(r, i, 2);
    row->name        = PQgetvalue(r, i, 3);
    row->entry_kind  = PQgetvalue(r, i, 4);
    const char *oid  = PQgetvalue(r, i, 5);
    if (oid && oid[0]) {
        snprintf(id_buf, VFS_OID_HEX_LEN + 1, "%s", oid);
        memcpy(row->object_id.hex, id_buf, VFS_OID_HEX_LEN + 1);
        row->has_object = 1;
    } else {
        memset(row->object_id.hex, 0, sizeof row->object_id.hex);
        row->has_object = 0;
    }
    row->ctime_ns = atoll(PQgetvalue(r, i, 6));
    row->mtime_ns = atoll(PQgetvalue(r, i, 7));
}

vfs_error vfs_mapping_list_view(vfs_store *s, const char *view,
                                vfs_mapping_cb cb, void *ud) {
    if (!s || !view || !cb) return VFS_ERR_BADARGS;
    char sql[1024];
    snprintf(sql, sizeof sql, "%s WHERE view_name = $1 ORDER BY view_path",
             MAPPING_SELECT);
    const char *params[1] = { view };
    PGresult *r = PQexecParams(s->pg, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        vfs_mapping_row row;
        char id_buf[VFS_OID_HEX_LEN + 1];
        fill_mapping_row(&row, r, i, id_buf);
        cb(&row, ud);
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_mapping_object_id(vfs_store *s, const char *view,
                                const char *view_path, vfs_object_id *out) {
    if (!s || !view || !view_path || !out) return VFS_ERR_BADARGS;
    vfs_canon_path cp;
    vfs_error rc = vfs_path_canonicalize(view_path, &cp);
    if (rc != VFS_OK) return rc;
    if (cp.is_root) return VFS_ERR_ISDIR;

    const char *params[2] = { view, cp.path };
    PGresult *r = PQexecParams(s->pg,
        "SELECT entry_kind, COALESCE(object_id, '') "
        "FROM mappings WHERE view_name = $1 AND view_path = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    if (PQntuples(r) == 0) { PQclear(r); return VFS_ERR_NOTFOUND; }

    const char *kind = PQgetvalue(r, 0, 0);
    const char *oid  = PQgetvalue(r, 0, 1);
    if (!strcmp(kind, "dir")) { PQclear(r); return VFS_ERR_ISDIR; }
    if (!oid || !oid[0])      { PQclear(r); return VFS_ERR_INTERNAL; }
    snprintf(out->hex, sizeof out->hex, "%s", oid);
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_mapping_list_object(vfs_store *s, const vfs_object_id *id,
                                  vfs_mapping_cb cb, void *ud) {
    if (!s || !id || !cb) return VFS_ERR_BADARGS;
    char sql[1024];
    snprintf(sql, sizeof sql,
             "%s WHERE object_id = $1 ORDER BY view_name, view_path",
             MAPPING_SELECT);
    const char *params[1] = { id->hex };
    PGresult *r = PQexecParams(s->pg, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        vfs_mapping_row row;
        char id_buf[VFS_OID_HEX_LEN + 1];
        fill_mapping_row(&row, r, i, id_buf);
        cb(&row, ud);
    }
    PQclear(r);
    return VFS_OK;
}
