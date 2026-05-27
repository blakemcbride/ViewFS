#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

vfs_error vfs_tag_add(vfs_store *s, const vfs_object_id *id, const char *tag) {
    if (!s || !id || !tag || !*tag) return VFS_ERR_BADARGS;
    int64_t now = vfs_now_ns();
    char now_s[24];
    snprintf(now_s, sizeof now_s, "%lld", (long long)now);
    const char *params[3] = { id->hex, tag, now_s };
    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO tags (object_id, tag, ctime_ns) "
        "VALUES ($1, $2, $3::bigint) "
        "ON CONFLICT (object_id, tag) DO NOTHING",
        3, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        vfs_seterr_pq(s, r); PQclear(r);
        if (code && !strcmp(code, "23503")) return VFS_ERR_NOTFOUND;
        return VFS_ERR_DB;
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_tag_remove(vfs_store *s, const vfs_object_id *id, const char *tag) {
    if (!s || !id || !tag) return VFS_ERR_BADARGS;
    const char *params[2] = { id->hex, tag };
    PGresult *r = PQexecParams(s->pg,
        "DELETE FROM tags WHERE object_id = $1 AND tag = $2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
}

vfs_error vfs_tag_list(vfs_store *s, const vfs_object_id *id,
                       vfs_tag_cb cb, void *ud) {
    if (!s || !id || !cb) return VFS_ERR_BADARGS;
    const char *params[1] = { id->hex };
    PGresult *r = PQexecParams(s->pg,
        "SELECT tag, ctime_ns FROM tags WHERE object_id = $1 ORDER BY tag",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        cb(PQgetvalue(r, i, 0), atoll(PQgetvalue(r, i, 1)), ud);
    }
    PQclear(r);
    return VFS_OK;
}
