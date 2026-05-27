#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* uid/gid in this library use -1 as the "leave NULL" sentinel. Convert
 * an int to either a printed integer or the literal NULL marker that
 * PQexecParams understands (a NULL char-pointer). */
static const char *opt_int_param(int v, char *buf, size_t bufsz) {
    if (v < 0) return NULL;
    snprintf(buf, bufsz, "%d", v);
    return buf;
}

/* Render the SHA intermediate state as lowercase hex into `hex_out`
 * (must hold at least 2*state_len + 1 bytes). Returns NULL when the
 * input is empty so PQexecParams sees a SQL NULL. The decode($N,'hex')
 * wrapper is added by the caller in the SQL string. */
static const char *opt_bytea_hex(const void *state, size_t state_len,
                                 char *hex_out, size_t hex_bufsz) {
    if (!state || state_len == 0) return NULL;
    if (state_len * 2 + 1 > hex_bufsz) return NULL;
    static const char HEX[] = "0123456789abcdef";
    const unsigned char *b = state;
    for (size_t i = 0; i < state_len; i++) {
        hex_out[2 * i]     = HEX[(b[i] >> 4) & 0xF];
        hex_out[2 * i + 1] = HEX[b[i] & 0xF];
    }
    hex_out[2 * state_len] = '\0';
    return hex_out;
}

/* Insert an objects row using a caller-supplied id. Used by `object import`
 * where the content file has already been written under that id. */
vfs_error vfs_object_insert_existing(vfs_store *s, const vfs_object_id *id,
                                     const char *kind, int mode, int64_t size,
                                     int uid, int gid,
                                     const char *checksum,
                                     const void *state, size_t state_len,
                                     int64_t ctime_ns, int64_t mtime_ns,
                                     int64_t atime_ns,
                                     const char *source_path) {
    if (!s || !id || !kind) return VFS_ERR_BADARGS;
    char mode_s[16], size_s[24], cs[24], ms[24], as[24];
    char uid_buf[16], gid_buf[16];
    char state_hex[VFS_SHA256_STATE_LEN * 2 + 1];
    snprintf(mode_s, sizeof mode_s, "%d",   mode);
    snprintf(size_s, sizeof size_s, "%lld", (long long)size);
    snprintf(cs,     sizeof cs,     "%lld", (long long)ctime_ns);
    snprintf(ms,     sizeof ms,     "%lld", (long long)mtime_ns);
    snprintf(as,     sizeof as,     "%lld", (long long)atime_ns);
    const char *params[12] = {
        id->hex, kind, mode_s, size_s, cs, ms, as,
        opt_int_param(uid, uid_buf, sizeof uid_buf),
        opt_int_param(gid, gid_buf, sizeof gid_buf),
        checksum,
        opt_bytea_hex(state, state_len, state_hex, sizeof state_hex),
        source_path
    };
    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO objects "
        "(object_id, kind, mode, size, ctime_ns, mtime_ns, atime_ns, "
        " uid, gid, checksum, checksum_state, source_path) "
        "VALUES ($1, $2, $3::int, $4::bigint, $5::bigint, $6::bigint, "
        "        $7::bigint, $8::int, $9::int, $10, "
        "        CASE WHEN $11::text IS NULL THEN NULL "
        "             ELSE decode($11, 'hex') END, $12)",
        12, NULL, params, NULL, NULL, 0);
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
                                 int            uid,
                                 int            gid,
                                 const char    *checksum,
                                 const void    *state,
                                 size_t         state_len,
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
    char uid_buf[16], gid_buf[16];
    char state_hex[VFS_SHA256_STATE_LEN * 2 + 1];
    snprintf(mode_s,  sizeof mode_s,  "%d",        mode);
    snprintf(size_s,  sizeof size_s,  "%lld",      (long long)size);
    snprintf(ctime_s, sizeof ctime_s, "%lld",      (long long)now);
    snprintf(mtime_s, sizeof mtime_s, "%lld",      (long long)mt);
    snprintf(atime_s, sizeof atime_s, "%lld",      (long long)now);

    const char *params[11] = {
        id.hex, mode_s, size_s, ctime_s, mtime_s, atime_s,
        opt_int_param(uid, uid_buf, sizeof uid_buf),
        opt_int_param(gid, gid_buf, sizeof gid_buf),
        checksum,
        opt_bytea_hex(state, state_len, state_hex, sizeof state_hex),
        source_path
    };
    PGresult *r = PQexecParams(s->pg,
        "INSERT INTO objects "
        "(object_id, kind, mode, size, ctime_ns, mtime_ns, atime_ns, "
        " uid, gid, checksum, checksum_state, source_path) "
        "VALUES ($1, 'file', $2::int, $3::bigint, $4::bigint, $5::bigint, "
        "        $6::bigint, $7::int, $8::int, $9, "
        "        CASE WHEN $10::text IS NULL THEN NULL "
        "             ELSE decode($10, 'hex') END, $11)",
        11, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    PQclear(r);
    *out_id = id;
    return VFS_OK;
}

vfs_error vfs_object_set_owner(vfs_store *s, const vfs_object_id *id,
                               int uid, int gid) {
    if (!s || !id) return VFS_ERR_BADARGS;
    if (uid < 0 && gid < 0) return VFS_OK;  /* nothing to update */
    char uid_buf[16], gid_buf[16];
    PGresult *r;
    if (uid >= 0 && gid >= 0) {
        snprintf(uid_buf, sizeof uid_buf, "%d", uid);
        snprintf(gid_buf, sizeof gid_buf, "%d", gid);
        const char *p[3] = { uid_buf, gid_buf, id->hex };
        r = PQexecParams(s->pg,
            "UPDATE objects SET uid = $1::int, gid = $2::int "
            "WHERE object_id = $3",
            3, NULL, p, NULL, NULL, 0);
    } else if (uid >= 0) {
        snprintf(uid_buf, sizeof uid_buf, "%d", uid);
        const char *p[2] = { uid_buf, id->hex };
        r = PQexecParams(s->pg,
            "UPDATE objects SET uid = $1::int WHERE object_id = $2",
            2, NULL, p, NULL, NULL, 0);
    } else {
        snprintf(gid_buf, sizeof gid_buf, "%d", gid);
        const char *p[2] = { gid_buf, id->hex };
        r = PQexecParams(s->pg,
            "UPDATE objects SET gid = $1::int WHERE object_id = $2",
            2, NULL, p, NULL, NULL, 0);
    }
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
}

vfs_error vfs_object_set_checksum(vfs_store *s, const vfs_object_id *id,
                                  const char *checksum_hex,
                                  const void *state, size_t state_len) {
    if (!s || !id) return VFS_ERR_BADARGS;
    char state_hex[VFS_SHA256_STATE_LEN * 2 + 1];
    const char *p[3] = {
        checksum_hex,
        opt_bytea_hex(state, state_len, state_hex, sizeof state_hex),
        id->hex
    };
    PGresult *r = PQexecParams(s->pg,
        "UPDATE objects SET checksum = $1, "
        "  checksum_state = CASE WHEN $2::text IS NULL THEN NULL "
        "                        ELSE decode($2, 'hex') END "
        "WHERE object_id = $3",
        3, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    int affected = atoi(PQcmdTuples(r));
    PQclear(r);
    return affected ? VFS_OK : VFS_ERR_NOTFOUND;
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
    b->info.uid = PQgetisnull(r, row, 9) ? -1 : atoi(PQgetvalue(r, row, 9));
    b->info.gid = PQgetisnull(r, row, 10) ? -1 : atoi(PQgetvalue(r, row, 10));
    b->info.has_checksum_state =
        !PQgetisnull(r, row, 11) && PQgetvalue(r, row, 11)[0] == 't';
}

static const char OBJ_SELECT[] =
    "SELECT object_id, kind, size, mode, ctime_ns, mtime_ns, atime_ns, "
    "       source_path, checksum, uid, gid, "
    "       (checksum_state IS NOT NULL) FROM objects";

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
