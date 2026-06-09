/* `viewfs find`: locate objects by property pairs. An object matches when
 * it satisfies ALL supplied pairs (AND). A pair with value==NULL matches any
 * value of the key. Implemented as one EXISTS clause per pair. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static const char OBJ_FIELDS[] =
    "o.object_id, o.kind, o.name, o.size, o.mode, o.ctime_ns, o.mtime_ns, "
    "o.atime_ns, o.source_path, o.checksum, o.uid, o.gid, "
    "(o.checksum_state IS NOT NULL)";

static void emit(PGresult *r, vfs_object_cb cb, void *ud) {
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        vfs_object_info info;
        memset(&info, 0, sizeof info);
        static char id_buf[VFS_OID_HEX_LEN + 1], kind_buf[16];
        static char name_buf[VFS_NAME_MAX + 1], src_buf[VFS_PATH_MAX], chk_buf[128];
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
}

vfs_error vfs_find_by_props(vfs_store *s, const vfs_prop_pair *pairs, int n,
                            vfs_object_cb cb, void *ud) {
    if (!s || !cb || n < 0 || (n > 0 && !pairs)) return VFS_ERR_BADARGS;

    int cap = 2 * n;
    const char **params = cap ? malloc((size_t)cap * sizeof *params) : NULL;
    if (cap && !params) return VFS_ERR_NOMEM;
    size_t sqlcap = 512 + (size_t)n * 160;
    char *sql = malloc(sqlcap);
    if (!sql) { free(params); return VFS_ERR_NOMEM; }

    int np = 0;
    int off = snprintf(sql, sqlcap, "SELECT %s FROM objects o WHERE 1=1", OBJ_FIELDS);
    for (int i = 0; i < n; i++) {
        if (!pairs[i].key) { free(sql); free(params); return VFS_ERR_BADARGS; }
        if (pairs[i].value) {
            off += snprintf(sql + off, sqlcap - (size_t)off,
                " AND EXISTS (SELECT 1 FROM object_props p "
                "WHERE p.object_id=o.object_id AND p.key=$%d AND p.value=$%d)",
                np + 1, np + 2);
            params[np++] = pairs[i].key;
            params[np++] = pairs[i].value;
        } else {
            off += snprintf(sql + off, sqlcap - (size_t)off,
                " AND EXISTS (SELECT 1 FROM object_props p "
                "WHERE p.object_id=o.object_id AND p.key=$%d)",
                np + 1);
            params[np++] = pairs[i].key;
        }
    }
    snprintf(sql + off, sqlcap - (size_t)off, " ORDER BY o.name, o.object_id");

    PGresult *r = PQexecParams(s->pg, sql, np, NULL, params, NULL, NULL, 0);
    free(sql);
    free(params);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    emit(r, cb, ud);
    PQclear(r);
    return VFS_OK;
}
