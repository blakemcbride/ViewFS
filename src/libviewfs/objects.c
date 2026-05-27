#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* Insert an objects row using a caller-supplied id. Used by `object import`
 * where the content file has already been written under that id. */
vfs_error vfs_object_insert_existing(vfs_store *s, const vfs_object_id *id,
                                     const char *kind, int mode, int64_t size,
                                     int64_t ctime_ns, int64_t mtime_ns,
                                     int64_t atime_ns,
                                     const char *source_path) {
    if (!s || !id || !kind) return VFS_ERR_BADARGS;
    char mode_s[16], size_s[24], cs[24], ms[24], as[24];
    snprintf(mode_s, sizeof mode_s, "%d",   mode);
    snprintf(size_s, sizeof size_s, "%lld", (long long)size);
    snprintf(cs,     sizeof cs,     "%lld", (long long)ctime_ns);
    snprintf(ms,     sizeof ms,     "%lld", (long long)mtime_ns);
    snprintf(as,     sizeof as,     "%lld", (long long)atime_ns);
    const char *params[8] = { id->hex, kind, mode_s, size_s, cs, ms, as, source_path };
    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO objects "
        "(object_id, kind, mode, size, ctime_ns, mtime_ns, atime_ns, source_path) "
        "VALUES ($1, $2, $3::int, $4::bigint, $5::bigint, $6::bigint, "
        "        $7::bigint, $8)",
        8, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        vfs_seterr_pq(s, r); PQclear(r);
        if (code && !strcmp(code, "23505")) return VFS_ERR_EXISTS;
        return VFS_ERR_DB;
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_object_create_file(vfs_store     *s,
                                 int            mode,
                                 int64_t        size,
                                 const char    *source_path,
                                 int64_t        mtime_ns,
                                 vfs_object_id *out_id) {
    if (!s || !out_id) return VFS_ERR_BADARGS;
    vfs_object_id id;
    vfs_error rc = vfs_object_id_generate(&id);
    if (rc != VFS_OK) return rc;

    int64_t now = vfs_now_ns();
    int64_t mt  = mtime_ns ? mtime_ns : now;

    char mode_s[16], size_s[24], ctime_s[24], mtime_s[24], atime_s[24];
    snprintf(mode_s,  sizeof mode_s,  "%d",        mode);
    snprintf(size_s,  sizeof size_s,  "%lld",      (long long)size);
    snprintf(ctime_s, sizeof ctime_s, "%lld",      (long long)now);
    snprintf(mtime_s, sizeof mtime_s, "%lld",      (long long)mt);
    snprintf(atime_s, sizeof atime_s, "%lld",      (long long)now);

    const char *params[7] = {
        id.hex, mode_s, size_s, ctime_s, mtime_s, atime_s, source_path
    };
    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO objects "
        "(object_id, kind, mode, size, ctime_ns, mtime_ns, atime_ns, source_path) "
        "VALUES ($1, 'file', $2::int, $3::bigint, $4::bigint, $5::bigint, "
        "        $6::bigint, $7)",
        7, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    PQclear(r);
    *out_id = id;
    return VFS_OK;
}

/* Caller-provided buffers for the strings inside vfs_object_info. */
typedef struct {
    vfs_object_info info;
    char id_buf[VFS_OID_HEX_LEN + 1];
    char kind_buf[16];
    char src_buf[VFS_PATH_MAX];
    char checksum_buf[128];
    int  src_present;
    int  checksum_present;
} obj_row_buf;

static void fill_object_info(obj_row_buf *b, PGresult *r, int row) {
    snprintf(b->id_buf, sizeof b->id_buf, "%s", PQgetvalue(r, row, 0));
    snprintf(b->kind_buf, sizeof b->kind_buf, "%s", PQgetvalue(r, row, 1));
    memcpy(b->info.id.hex, b->id_buf, sizeof b->info.id.hex);
    b->info.kind     = b->kind_buf;
    b->info.size     = atoll(PQgetvalue(r, row, 2));
    b->info.mode     = atoi(PQgetvalue(r, row, 3));
    b->info.ctime_ns = atoll(PQgetvalue(r, row, 4));
    b->info.mtime_ns = atoll(PQgetvalue(r, row, 5));
    b->info.atime_ns = atoll(PQgetvalue(r, row, 6));
    if (PQgetisnull(r, row, 7)) {
        b->info.source_path = NULL;
    } else {
        snprintf(b->src_buf, sizeof b->src_buf, "%s", PQgetvalue(r, row, 7));
        b->info.source_path = b->src_buf;
    }
    if (PQgetisnull(r, row, 8)) {
        b->info.checksum = NULL;
    } else {
        snprintf(b->checksum_buf, sizeof b->checksum_buf, "%s",
                 PQgetvalue(r, row, 8));
        b->info.checksum = b->checksum_buf;
    }
}

static const char OBJ_SELECT[] =
    "SELECT object_id, kind, size, mode, ctime_ns, mtime_ns, atime_ns, "
    "       source_path, checksum FROM objects";

vfs_error vfs_object_get(vfs_store *s, const vfs_object_id *id,
                         vfs_object_info *out) {
    if (!s || !id || !out) return VFS_ERR_BADARGS;
    char sql[256];
    snprintf(sql, sizeof sql, "%s WHERE object_id = $1", OBJ_SELECT);
    const char *params[1] = { id->hex };
    PGresult *r = PQexecParams(s->pg, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    if (PQntuples(r) == 0) { PQclear(r); return VFS_ERR_NOTFOUND; }
    static obj_row_buf b;
    fill_object_info(&b, r, 0);
    *out = b.info;
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_object_resolve(vfs_store *s, const char *prefix, vfs_object_id *out) {
    if (!s || !prefix || !out) return VFS_ERR_BADARGS;
    size_t n = strlen(prefix);
    if (n == 0 || n > VFS_OID_HEX_LEN) return VFS_ERR_BADARGS;
    for (size_t i = 0; i < n; i++) {
        char c = prefix[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return VFS_ERR_BADARGS;
    }
    if (n == VFS_OID_HEX_LEN) {
        memcpy(out->hex, prefix, VFS_OID_HEX_LEN);
        out->hex[VFS_OID_HEX_LEN] = '\0';
        vfs_object_info info;
        vfs_error rc = vfs_object_get(s, out, &info);
        return rc;
    }
    char upper[VFS_OID_HEX_LEN + 1];
    memcpy(upper, prefix, n);
    /* upper bound for range: prefix with last char incremented. The simple
     * approach is to append 'g' (one past 'f') and rely on LIKE-style range. */
    upper[n] = 'g';
    upper[n + 1] = '\0';
    const char *params[2] = { prefix, upper };
    PGresult *r = PQexecParams(s->pg,
        "SELECT object_id FROM objects "
        "WHERE object_id >= $1 AND object_id < $2 LIMIT 2",
        2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int nr = PQntuples(r);
    if (nr == 0) { PQclear(r); return VFS_ERR_NOTFOUND; }
    if (nr > 1)  { PQclear(r); return VFS_ERR_AMBIGUOUS; }
    snprintf(out->hex, sizeof out->hex, "%s", PQgetvalue(r, 0, 0));
    PQclear(r);
    return VFS_OK;
}

static vfs_error object_list_internal(vfs_store *s, const char *where_clause,
                                      vfs_object_cb cb, void *ud) {
    if (!cb) return VFS_ERR_BADARGS;
    char sql[1024];
    snprintf(sql, sizeof sql, "%s %s ORDER BY object_id",
             OBJ_SELECT, where_clause ? where_clause : "");
    PGresult *r = PQexec(s->pg, sql);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        obj_row_buf b;
        fill_object_info(&b, r, i);
        cb(&b.info, ud);
    }
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_object_list(vfs_store *s, vfs_object_cb cb, void *ud) {
    return object_list_internal(s, "", cb, ud);
}

vfs_error vfs_object_list_orphans(vfs_store *s, vfs_object_cb cb, void *ud) {
    return object_list_internal(s,
        "WHERE NOT EXISTS (SELECT 1 FROM mappings m "
        "                  WHERE m.object_id = objects.object_id)",
        cb, ud);
}

vfs_error vfs_object_delete(vfs_store *s, const vfs_object_id *id) {
    if (!s || !id) return VFS_ERR_BADARGS;
    const char *params[1] = { id->hex };
    PGresult *r = PQexecParams(s->pg,
        "DELETE FROM objects WHERE object_id = $1",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    if (!affected) return VFS_ERR_NOTFOUND;
    return vfs_content_unlink(s, id);
}
