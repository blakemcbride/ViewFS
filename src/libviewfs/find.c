#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static const char OBJ_FIELDS[] =
    "o.object_id, o.kind, o.size, o.mode, o.ctime_ns, o.mtime_ns, "
    "o.atime_ns, o.source_path, o.checksum";

/* The same row shape as objects.c expects, but joined via tag/attr. */
static vfs_error find_emit(vfs_store *s, PGresult *r, vfs_object_cb cb, void *ud) {
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        vfs_object_info info;
        memset(&info, 0, sizeof info);

        static char id_buf[VFS_OID_HEX_LEN + 1];
        static char kind_buf[16];
        static char src_buf[VFS_PATH_MAX];
        static char chk_buf[128];

        snprintf(id_buf,   sizeof id_buf,   "%s", PQgetvalue(r, i, 0));
        snprintf(kind_buf, sizeof kind_buf, "%s", PQgetvalue(r, i, 1));
        memcpy(info.id.hex, id_buf, sizeof info.id.hex);
        info.kind     = kind_buf;
        info.size     = atoll(PQgetvalue(r, i, 2));
        info.mode     = atoi(PQgetvalue(r, i, 3));
        info.ctime_ns = atoll(PQgetvalue(r, i, 4));
        info.mtime_ns = atoll(PQgetvalue(r, i, 5));
        info.atime_ns = atoll(PQgetvalue(r, i, 6));
        if (!PQgetisnull(r, i, 7)) {
            snprintf(src_buf, sizeof src_buf, "%s", PQgetvalue(r, i, 7));
            info.source_path = src_buf;
        }
        if (!PQgetisnull(r, i, 8)) {
            snprintf(chk_buf, sizeof chk_buf, "%s", PQgetvalue(r, i, 8));
            info.checksum = chk_buf;
        }
        cb(&info, ud);
    }
    (void)s;
    return VFS_OK;
}

vfs_error vfs_find_by_tag(vfs_store *s, const char *tag,
                          vfs_object_cb cb, void *ud) {
    if (!s || !tag || !cb) return VFS_ERR_BADARGS;
    char sql[1024];
    snprintf(sql, sizeof sql,
        "SELECT %s FROM objects o "
        "JOIN tags t ON t.object_id = o.object_id "
        "WHERE t.tag = $1 ORDER BY o.object_id", OBJ_FIELDS);
    const char *params[1] = { tag };
    PGresult *r = PQexecParams(s->pg, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    vfs_error rc = find_emit(s, r, cb, ud);
    PQclear(r);
    return rc;
}

vfs_error vfs_find_by_attr(vfs_store *s, const char *key, const char *value,
                           vfs_object_cb cb, void *ud) {
    if (!s || !key || !cb) return VFS_ERR_BADARGS;
    char sql[1024];
    if (value) {
        snprintf(sql, sizeof sql,
            "SELECT %s FROM objects o "
            "JOIN attributes a ON a.object_id = o.object_id "
            "WHERE a.key = $1 AND a.value = $2 ORDER BY o.object_id",
            OBJ_FIELDS);
        const char *params[2] = { key, value };
        PGresult *r = PQexecParams(s->pg, sql, 2, NULL, params, NULL, NULL, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
        }
        vfs_error rc = find_emit(s, r, cb, ud);
        PQclear(r);
        return rc;
    } else {
        snprintf(sql, sizeof sql,
            "SELECT %s FROM objects o "
            "JOIN attributes a ON a.object_id = o.object_id "
            "WHERE a.key = $1 ORDER BY o.object_id",
            OBJ_FIELDS);
        const char *params[1] = { key };
        PGresult *r = PQexecParams(s->pg, sql, 1, NULL, params, NULL, NULL, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
        }
        vfs_error rc = find_emit(s, r, cb, ud);
        PQclear(r);
        return rc;
    }
}
