#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

/* ------------------------------------------------------------------
 * Embedded migration SQL.
 *
 * IMPORTANT: keep this in sync with src/libviewfs/migrations/0001_init.sql.
 * The .sql file is the human-readable canonical form; this string is what
 * actually runs. They differ only in C-string escaping.
 * ------------------------------------------------------------------ */
static const char MIGRATION_0001[] =
    "CREATE TABLE schema_migrations ("
    "  version    INTEGER PRIMARY KEY,"
    "  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()"
    ");"
    "CREATE TABLE objects ("
    "  object_id      TEXT PRIMARY KEY,"
    "  kind           TEXT NOT NULL CHECK (kind IN ('file','symlink')),"
    "  name           TEXT NOT NULL DEFAULT '',"
    "  size           BIGINT NOT NULL DEFAULT 0,"
    "  mode           INTEGER NOT NULL,"
    "  uid            INTEGER,"
    "  gid            INTEGER,"
    "  ctime_ns       BIGINT NOT NULL,"
    "  mtime_ns       BIGINT NOT NULL,"
    "  atime_ns       BIGINT NOT NULL,"
    "  checksum       TEXT,"
    "  checksum_state BYTEA,"
    "  source_path    TEXT,"
    "  symlink_target TEXT"
    ");"
    "CREATE TABLE object_props ("
    "  object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,"
    "  key       TEXT NOT NULL,"
    "  value     TEXT NOT NULL,"
    "  ctime_ns  BIGINT NOT NULL,"
    "  mtime_ns  BIGINT NOT NULL,"
    "  PRIMARY KEY (object_id, key, value)"
    ");"
    "CREATE INDEX object_props_kv  ON object_props (key, value);"
    "CREATE INDEX object_props_obj ON object_props (object_id);"
    "CREATE TABLE views ("
    "  view_name   TEXT PRIMARY KEY,"
    "  description TEXT,"
    "  ctime_ns    BIGINT NOT NULL,"
    "  mtime_ns    BIGINT NOT NULL"
    ");"
    "CREATE TABLE view_dirs ("
    "  view_name   TEXT NOT NULL REFERENCES views(view_name) ON DELETE CASCADE,"
    "  dir_path    TEXT NOT NULL,"
    "  parent_path TEXT NOT NULL,"
    "  name        TEXT NOT NULL,"
    "  mode        INTEGER NOT NULL,"
    "  ctime_ns    BIGINT NOT NULL,"
    "  mtime_ns    BIGINT NOT NULL,"
    "  PRIMARY KEY (view_name, dir_path)"
    ");"
    "CREATE INDEX view_dirs_parent ON view_dirs (view_name, parent_path);"
    "CREATE TABLE view_dir_props ("
    "  view_name TEXT NOT NULL,"
    "  dir_path  TEXT NOT NULL,"
    "  key       TEXT NOT NULL,"
    "  value     TEXT NOT NULL,"
    "  flow      BOOLEAN NOT NULL DEFAULT FALSE,"
    "  ctime_ns  BIGINT NOT NULL,"
    "  mtime_ns  BIGINT NOT NULL,"
    "  PRIMARY KEY (view_name, dir_path, key, value),"
    "  FOREIGN KEY (view_name, dir_path)"
    "    REFERENCES view_dirs(view_name, dir_path)"
    "    ON DELETE CASCADE ON UPDATE CASCADE"
    ");"
    "CREATE INDEX view_dir_props_flow ON view_dir_props (view_name, flow);";

/* ------------------------------------------------------------------
 * Common helpers
 * ------------------------------------------------------------------ */

void vfs_seterr(vfs_store *s, const char *fmt, ...) {
    if (!s) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
    va_end(ap);
}

void vfs_seterr_pq(vfs_store *s, const PGresult *r) {
    if (!s) return;
    const char *msg = NULL;
    if (r) msg = PQresultErrorMessage(r);
    if (!msg || !*msg) msg = PQerrorMessage(s->pg);
    if (!msg) msg = "(no error message)";
    snprintf(s->last_error, sizeof s->last_error, "%s", msg);
    /* trim trailing newline */
    size_t n = strlen(s->last_error);
    while (n > 0 && (s->last_error[n-1] == '\n' || s->last_error[n-1] == '\r'))
        s->last_error[--n] = '\0';
}

int vfs_ident_ok(const char *s) {
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 1; s[i]; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '_')) return 0;
        if (i >= 62) return 0;
    }
    return 1;
}

int64_t vfs_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

vfs_error vfs_exec_simple(vfs_store *s, const char *sql) {
    PGresult *r = PQexec(s->pg, sql);
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r);
        PQclear(r);
        return VFS_ERR_DB;
    }
    PQclear(r);
    return VFS_OK;
}

/* ------------------------------------------------------------------
 * Backing-store directory skeleton
 * ------------------------------------------------------------------ */

static int mkdir_if_missing(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

vfs_error vfs_store_mkskel(const char *store_path) {
    if (mkdir_if_missing(store_path, 0755) != 0) return VFS_ERR_IO;
    char buf[VFS_PATH_MAX];
    const char *subs[] = {
        VFS_OBJECTS_DIR, VFS_TMP_DIR, VFS_DAEMONS_DIR, VFS_LOGS_DIR, NULL,
    };
    for (size_t i = 0; subs[i]; i++) {
        int n = snprintf(buf, sizeof buf, "%s/%s", store_path, subs[i]);
        if (n < 0 || (size_t)n >= sizeof buf) return VFS_ERR_IO;
        if (mkdir_if_missing(buf, 0755) != 0) return VFS_ERR_IO;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------
 * Migrations
 * ------------------------------------------------------------------ */

vfs_error vfs_apply_migrations(vfs_store *s) {
    /* Check whether schema_migrations exists in the current search_path. */
    PGresult *r = PQexec(s->pg,
        "SELECT to_regclass(current_schema() || '.schema_migrations') IS NOT NULL");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r);
        PQclear(r);
        return VFS_ERR_DB;
    }
    int has_table = (PQntuples(r) == 1 && PQgetvalue(r, 0, 0)[0] == 't');
    PQclear(r);

    int applied_v = 0;
    if (has_table) {
        r = PQexec(s->pg, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations");
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            vfs_seterr_pq(s, r);
            PQclear(r);
            return VFS_ERR_DB;
        }
        applied_v = atoi(PQgetvalue(r, 0, 0));
        PQclear(r);
    }

    if (applied_v >= VIEWFS_SCHEMA_VERSION) return VFS_OK;

    /* Apply migration 0001 in one transaction. */
    if (applied_v < 1) {
        vfs_error rc = vfs_exec_simple(s, "BEGIN");
        if (rc != VFS_OK) return rc;
        rc = vfs_exec_simple(s, MIGRATION_0001);
        if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }
        rc = vfs_exec_simple(s,
            "INSERT INTO schema_migrations (version) VALUES (1)");
        if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }
        rc = vfs_exec_simple(s, "COMMIT");
        if (rc != VFS_OK) return rc;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------
 * Store lifecycle
 * ------------------------------------------------------------------ */

/* Open a connection to a maintenance database (`maint_db`) using every
 * field of `opts` except dbname, which is overridden. Returns NULL on
 * allocation failure; the caller checks PQstatus on a non-NULL result. */
static PGconn *connect_maintenance(PQconninfoOption *opts, const char *maint_db) {
    int n = 0;
    for (PQconninfoOption *o = opts; o->keyword; o++) n++;
    const char **kw  = calloc((size_t)n + 2, sizeof *kw);
    const char **val = calloc((size_t)n + 2, sizeof *val);
    if (!kw || !val) { free(kw); free(val); return NULL; }
    int i = 0, have_db = 0;
    for (PQconninfoOption *o = opts; o->keyword; o++) {
        if (!o->val || !*o->val) continue;
        kw[i] = o->keyword;
        if (!strcmp(o->keyword, "dbname")) { val[i] = maint_db; have_db = 1; }
        else                               { val[i] = o->val; }
        i++;
    }
    if (!have_db) { kw[i] = "dbname"; val[i] = maint_db; i++; }
    kw[i] = NULL; val[i] = NULL;
    PGconn *m = PQconnectdbParams(kw, val, 0);
    free(kw); free(val);
    return m;
}

/* Create the database named in s->conninfo (used by `init` when the target
 * database does not yet exist). Connects to a maintenance database with the
 * same host/user/etc. and issues CREATE DATABASE. */
static vfs_error try_create_database(vfs_store *s) {
    char *errmsg = NULL;
    PQconninfoOption *opts = PQconninfoParse(s->conninfo[0] ? s->conninfo : "",
                                            &errmsg);
    if (!opts) {
        vfs_seterr(s, "cannot parse conninfo: %s", errmsg ? errmsg : "(unknown)");
        if (errmsg) PQfreemem(errmsg);
        return VFS_ERR_DB;
    }
    const char *dbname = NULL;
    for (PQconninfoOption *o = opts; o->keyword; o++)
        if (!strcmp(o->keyword, "dbname") && o->val && *o->val) dbname = o->val;
    if (!dbname) {
        vfs_seterr(s, "conninfo specifies no dbname; cannot create database");
        PQconninfoFree(opts);
        return VFS_ERR_DB;
    }

    const char *maint[] = { "postgres", "template1" };
    vfs_error rc = VFS_ERR_DB;
    for (size_t k = 0; k < sizeof maint / sizeof maint[0]; k++) {
        PGconn *m = connect_maintenance(opts, maint[k]);
        if (!m) continue;
        if (PQstatus(m) != CONNECTION_OK) { PQfinish(m); continue; }
        char *qid = PQescapeIdentifier(m, dbname, strlen(dbname));
        if (!qid) { PQfinish(m); continue; }
        char sql[512];
        snprintf(sql, sizeof sql, "CREATE DATABASE %s", qid);
        PQfreemem(qid);
        PGresult *r = PQexec(m, sql);
        const char *code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        if (PQresultStatus(r) == PGRES_COMMAND_OK ||
            (code && !strcmp(code, "42P04"))) {   /* 42P04 = duplicate_database */
            rc = VFS_OK;
        } else {
            vfs_seterr(s, "CREATE DATABASE failed: %s", PQresultErrorMessage(r));
        }
        PQclear(r);
        PQfinish(m);
        if (rc == VFS_OK) break;
    }
    PQconninfoFree(opts);
    return rc;
}

/* Connect to s->conninfo. When create_db is set and the initial connect
 * fails (e.g. the database does not exist), attempt to CREATE DATABASE via a
 * maintenance connection and retry once. */
static vfs_error connect_pq_ex(vfs_store *s, int create_db) {
    const char *ci = s->conninfo[0] ? s->conninfo : NULL;
    PGconn *pg = PQconnectdb(ci ? ci : "");
    if (PQstatus(pg) == CONNECTION_OK) { s->pg = pg; return VFS_OK; }

    if (create_db) {
        char first_err[512];
        snprintf(first_err, sizeof first_err, "%s", PQerrorMessage(pg));
        PQfinish(pg);
        if (try_create_database(s) == VFS_OK) {
            pg = PQconnectdb(ci ? ci : "");
            if (PQstatus(pg) == CONNECTION_OK) { s->pg = pg; return VFS_OK; }
            vfs_seterr(s, "connect after CREATE DATABASE failed: %s",
                       PQerrorMessage(pg));
            PQfinish(pg);
            return VFS_ERR_DB;
        }
        /* try_create_database set a message; prefer the original connect
         * error if it is more informative. */
        if (s->last_error[0] == '\0')
            vfs_seterr(s, "PostgreSQL connect failed: %s", first_err);
        return VFS_ERR_DB;
    }

    vfs_seterr(s, "PostgreSQL connect failed: %s", PQerrorMessage(pg));
    PQfinish(pg);
    return VFS_ERR_DB;
}

static vfs_error connect_pq(vfs_store *s) { return connect_pq_ex(s, 0); }

static vfs_error set_search_path(vfs_store *s) {
    char sql[256];
    /* schema name already validated via vfs_ident_ok */
    snprintf(sql, sizeof sql, "SET search_path TO \"%s\"", s->schema);
    return vfs_exec_simple(s, sql);
}

static vfs_error create_schema_if_needed(vfs_store *s) {
    char sql[256];
    snprintf(sql, sizeof sql, "CREATE SCHEMA IF NOT EXISTS \"%s\"", s->schema);
    return vfs_exec_simple(s, sql);
}

vfs_error vfs_store_create(const char *store_path,
                           const char *pg_conninfo,
                           const char *pg_schema,
                           int         reinit) {
    if (!store_path) return VFS_ERR_BADARGS;

    vfs_store local;
    memset(&local, 0, sizeof local);
    snprintf(local.store_path, sizeof local.store_path, "%s", store_path);
    snprintf(local.schema, sizeof local.schema, "%s",
             (pg_schema && *pg_schema) ? pg_schema : VFS_DEFAULT_SCHEMA);
    if (!vfs_ident_ok(local.schema)) return VFS_ERR_BADARGS;
    if (pg_conninfo)
        snprintf(local.conninfo, sizeof local.conninfo, "%s", pg_conninfo);
    local.store_version = 1;
    local.shard_depth   = VFS_SHARD_DEPTH;

    char cfg[VFS_PATH_MAX];
    int n = snprintf(cfg, sizeof cfg, "%s/%s", store_path, VFS_CONFIG_FILE);
    if (n < 0 || (size_t)n >= sizeof cfg) return VFS_ERR_IO;
    if (!reinit && access(cfg, F_OK) == 0) return VFS_ERR_EXISTS;

    vfs_error rc = vfs_store_mkskel(store_path);
    if (rc != VFS_OK) return rc;

    /* `init` creates the database too: if the target db does not exist,
     * connect_pq_ex connects to a maintenance db and CREATE DATABASEs it. */
    rc = connect_pq_ex(&local, 1);
    if (rc != VFS_OK) {
        /* propagate error message via stderr by storing in a static buf */
        fprintf(stderr, "viewfs: %s\n", local.last_error);
        return rc;
    }

    rc = create_schema_if_needed(&local);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs: %s\n", local.last_error);
        PQfinish(local.pg);
        return rc;
    }
    rc = set_search_path(&local);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs: %s\n", local.last_error);
        PQfinish(local.pg);
        return rc;
    }
    rc = vfs_apply_migrations(&local);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs: %s\n", local.last_error);
        PQfinish(local.pg);
        return rc;
    }

    rc = vfs_store_save_config(&local);
    PQfinish(local.pg);
    return rc;
}

vfs_error vfs_store_open(const char *store_path, vfs_store **out) {
    if (!store_path || !out) return VFS_ERR_BADARGS;
    vfs_store *s = calloc(1, sizeof *s);
    if (!s) return VFS_ERR_NOMEM;
    snprintf(s->store_path, sizeof s->store_path, "%s", store_path);

    vfs_error rc = vfs_store_load_config(s);
    if (rc != VFS_OK) {
        if (s->last_error[0] == '\0')
            vfs_seterr(s, "could not load %s/%s", store_path, VFS_CONFIG_FILE);
        /* keep s alive for caller to inspect last_error via... actually
         * we have no API exposing s on failure. Print the message and free. */
        fprintf(stderr, "viewfs: %s\n", s->last_error);
        free(s);
        return rc;
    }
    rc = connect_pq(s);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs: %s\n", s->last_error);
        free(s);
        return rc;
    }
    rc = set_search_path(s);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs: %s\n", s->last_error);
        PQfinish(s->pg);
        free(s);
        return rc;
    }
    rc = vfs_apply_migrations(s);
    if (rc != VFS_OK) {
        fprintf(stderr, "viewfs: %s\n", s->last_error);
        PQfinish(s->pg);
        free(s);
        return rc;
    }
    *out = s;
    return VFS_OK;
}

void vfs_store_close(vfs_store *s) {
    if (!s) return;
    if (s->pg) PQfinish(s->pg);
    free(s);
}

const char *vfs_store_last_error(vfs_store *s) {
    if (!s) return "(no store)";
    return s->last_error[0] ? s->last_error : "(no error)";
}

const char *vfs_store_path    (const vfs_store *s) { return s ? s->store_path : ""; }
const char *vfs_store_schema  (const vfs_store *s) { return s ? s->schema : ""; }
const char *vfs_store_conninfo(const vfs_store *s) { return s ? s->conninfo : ""; }

vfs_error vfs_schema_version(vfs_store *s, int *out) {
    if (!s || !out) return VFS_ERR_BADARGS;
    PGresult *r = PQexec(s->pg,
        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    *out = atoi(PQgetvalue(r, 0, 0));
    PQclear(r);
    return VFS_OK;
}

static vfs_error count_query(vfs_store *s, const char *sql, int64_t *out) {
    PGresult *r = PQexec(s->pg, sql);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    *out = atoll(PQgetvalue(r, 0, 0));
    PQclear(r);
    return VFS_OK;
}

vfs_error vfs_check_dirs_reachable(vfs_store *s, int64_t *out) {
    if (!s || !out) return VFS_ERR_BADARGS;
    /* Count directory rows whose parent_path names a directory that does
     * not exist in the same view (a broken tree). Root rows have
     * parent_path='' and are excluded. In a healthy store this is 0. */
    return count_query(s,
        "SELECT count(*) FROM view_dirs d "
        "WHERE d.parent_path <> '' "
        "  AND NOT EXISTS (SELECT 1 FROM view_dirs p "
        "                  WHERE p.view_name = d.view_name "
        "                    AND p.dir_path  = d.parent_path)",
        out);
}

vfs_error vfs_check_dir_structure(vfs_store *s, int64_t *out) {
    if (!s || !out) return VFS_ERR_BADARGS;
    /* A directory row is structurally consistent when its name is the final
     * path component and its parent_path is the dirname (or '/' for a
     * top-level dir; '' only for the root). Counts violators. */
    return count_query(s,
        "SELECT count(*) FROM view_dirs WHERE NOT ("
        "  (dir_path = '/' AND parent_path = '' AND name = '') OR "
        "  (dir_path <> '/' "
        "   AND name = substring(dir_path from '[^/]+$') "
        "   AND ( (position('/' in substring(dir_path from 2)) = 0 "
        "          AND parent_path = '/') "
        "         OR parent_path = substring(dir_path from 1 "
        "                          for length(dir_path) - length(name) - 1) )))",
        out);
}

vfs_error vfs_check_props_orphans(vfs_store *s, int64_t *out) {
    if (!s || !out) return VFS_ERR_BADARGS;
    /* object_props rows referencing a missing object. The FK makes this
     * impossible barring DB tampering, but the scan is cheap. */
    return count_query(s,
        "SELECT count(*) FROM object_props p "
        "WHERE NOT EXISTS (SELECT 1 FROM objects o "
        "                  WHERE o.object_id = p.object_id)",
        out);
}

vfs_error vfs_count_rows(vfs_store *s, const char *table_id, int64_t *out) {
    if (!s || !out || !table_id) return VFS_ERR_BADARGS;
    /* table_id is an enum-like constant; whitelist to avoid SQL injection
     * via the FROM clause. */
    static const char *const allowed[] = {
        "objects", "object_props", "views", "view_dirs", "view_dir_props", NULL,
    };
    int ok = 0;
    for (size_t i = 0; allowed[i]; i++) {
        if (!strcmp(table_id, allowed[i])) { ok = 1; break; }
    }
    if (!ok) return VFS_ERR_BADARGS;
    char sql[64];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", table_id);
    PGresult *r = PQexec(s->pg, sql);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        vfs_seterr_pq(s, r); PQclear(r); return VFS_ERR_DB;
    }
    *out = atoll(PQgetvalue(r, 0, 0));
    PQclear(r);
    return VFS_OK;
}
