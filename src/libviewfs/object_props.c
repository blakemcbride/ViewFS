/* Object properties: multi-valued (key,value) pairs that drive view
 * membership. Replaces the old attributes + tags tables. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

vfs_error vfs_object_prop_add(vfs_store *s, const vfs_object_id *id,
                              const char *key, const char *value) {
    if (!s || !id || !key || !*key || !value) return VFS_ERR_BADARGS;
    int64_t now = vfs_now_ns();
    char now_s[24];
    snprintf(now_s, sizeof now_s, "%lld", (long long)now);
    const char *params[5] = { id->hex, key, value, now_s, now_s };
    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO object_props (object_id, key, value, ctime_ns, mtime_ns) "
        "VALUES ($1, $2, $3, $4::bigint, $5::bigint) "
        "ON CONFLICT (object_id, key, value) DO NOTHING",
        5, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        vfs_seterr_pq(s, r); PQclear(r);
        if (code && !strcmp(code, "23503")) return VFS_ERR_NOTFOUND;
        return VFS_ERR_DB;
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_object_prop_unset(vfs_store *s, const vfs_object_id *id,
                                const char *key, const char *value) {
    if (!s || !id || !key) return VFS_ERR_BADARGS;
    PGresult *r;
    if (value) {
        const char *params[3] = { id->hex, key, value };
        r = PQexecParams(s->pg,
            "DELETE FROM object_props "
            "WHERE object_id = $1 AND key = $2 AND value = $3",
            3, NULL, params, NULL, NULL, 0);
    } else {
        const char *params[2] = { id->hex, key };
        r = PQexecParams(s->pg,
            "DELETE FROM object_props WHERE object_id = $1 AND key = $2",
            2, NULL, params, NULL, NULL, 0);
    }
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
}

vfs_error vfs_object_prop_list(vfs_store *s, const vfs_object_id *id,
                               vfs_prop_cb cb, void *ud) {
    if (!s || !id || !cb) return VFS_ERR_BADARGS;
    const char *params[1] = { id->hex };
    PGresult *r = PQexecParams(s->pg,
        "SELECT key, value, ctime_ns, mtime_ns FROM object_props "
        "WHERE object_id = $1 ORDER BY key, value",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        vfs_prop_row row;
        row.key      = PQgetvalue(r, i, 0);
        row.value    = PQgetvalue(r, i, 1);
        row.ctime_ns = atoll(PQgetvalue(r, i, 2));
        row.mtime_ns = atoll(PQgetvalue(r, i, 3));
        cb(&row, ud);
    }
    PQclear(r);
    return VFS_OK;
}
