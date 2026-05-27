#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

vfs_error vfs_view_create(vfs_store *s, const char *name, const char *description) {
    if (!s || !name || !*name) return VFS_ERR_BADARGS;
    int64_t now = vfs_now_ns();
    char now_s[32];
    snprintf(now_s, sizeof now_s, "%lld", (long long)now);

    const char *params[4] = { name, description, now_s, now_s };
    int   lengths[4] = { 0, 0, 0, 0 };
    int   formats[4] = { 0, 0, 0, 0 };

    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO views (view_name, description, ctime_ns, mtime_ns) "
        "VALUES ($1, $2, $3::bigint, $4::bigint)",
        4, NULL, params, lengths, formats, 0);
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK) {
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        vfs_seterr_pq(s, r);
        PQclear(r);
        if (code && !strcmp(code, "23505")) return VFS_ERR_EXISTS;
        return VFS_ERR_DB;
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_view_delete(vfs_store *s, const char *name) {
    if (!s || !name) return VFS_ERR_BADARGS;
    const char *params[1] = { name };
    PGresult *r = PQexecParams(s->pg,
        "DELETE FROM views WHERE view_name = $1",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    if (affected) vfs_notify_path(s, name, "");
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
}

vfs_error vfs_view_list(vfs_store *s, vfs_view_cb cb, void *ud) {
    if (!s || !cb) return VFS_ERR_BADARGS;
    PGresult *r = PQexec(s->pg,
        "SELECT view_name, description, ctime_ns, mtime_ns "
        "FROM views ORDER BY view_name");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        vfs_view_row row;
        row.name        = PQgetvalue(r, i, 0);
        row.description = PQgetisnull(r, i, 1) ? NULL : PQgetvalue(r, i, 1);
        row.ctime_ns    = atoll(PQgetvalue(r, i, 2));
        row.mtime_ns    = atoll(PQgetvalue(r, i, 3));
        cb(&row, ud);
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_view_exists(vfs_store *s, const char *name, int *out_exists) {
    if (!s || !name || !out_exists) return VFS_ERR_BADARGS;
    const char *params[1] = { name };
    PGresult *r = PQexecParams(s->pg,
        "SELECT 1 FROM views WHERE view_name = $1",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    *out_exists = PQntuples(r) > 0;
    PQclear(r);
    return VFS_OK;
}
