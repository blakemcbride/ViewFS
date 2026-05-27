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
    "  size           BIGINT NOT NULL DEFAULT 0,"
    "  mode           INTEGER NOT NULL,"
    "  uid            INTEGER,"
    "  gid            INTEGER,"
    "  ctime_ns       BIGINT NOT NULL,"
    "  mtime_ns       BIGINT NOT NULL,"
    "  atime_ns       BIGINT NOT NULL,"
    "  checksum       TEXT,"
    "  source_path    TEXT,"
    "  symlink_target TEXT"
    ");"
    "CREATE TABLE views ("
    "  view_name   TEXT PRIMARY KEY,"
    "  description TEXT,"
    "  ctime_ns    BIGINT NOT NULL,"
    "  mtime_ns    BIGINT NOT NULL"
    ");"
    "CREATE TABLE mappings ("
    "  view_name     TEXT NOT NULL REFERENCES views(view_name) ON DELETE CASCADE,"
    "  view_path     TEXT NOT NULL,"
    "  parent_path   TEXT NOT NULL,"
    "  name          TEXT NOT NULL,"
    "  entry_kind    TEXT NOT NULL CHECK (entry_kind IN ('file','dir','symlink')),"
    "  object_id     TEXT REFERENCES objects(object_id) ON DELETE SET NULL,"
    "  mode_override INTEGER,"
    "  ctime_ns      BIGINT NOT NULL,"
    "  mtime_ns      BIGINT NOT NULL,"
    "  PRIMARY KEY (view_name, view_path),"
    "  CHECK ( (entry_kind = 'dir') = (object_id IS NULL) )"
    ");"
    "CREATE INDEX mappings_parent ON mappings (view_name, parent_path);"
    "CREATE INDEX mappings_object ON mappings (object_id);"
    "CREATE TABLE attributes ("
    "  object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,"
    "  key       TEXT NOT NULL,"
    "  value     TEXT NOT NULL,"
    "  ctime_ns  BIGINT NOT NULL,"
    "  mtime_ns  BIGINT NOT NULL,"
    "  PRIMARY KEY (object_id, key)"
    ");"
    "CREATE INDEX attributes_kv ON attributes (key, value);"
    "CREATE TABLE tags ("
    "  object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,"
    "  tag       TEXT NOT NULL,"
    "  ctime_ns  BIGINT NOT NULL,"
    "  PRIMARY KEY (object_id, tag)"
    ");"
    "CREATE INDEX tags_tag ON tags (tag);";

/* Keep in sync with src/libviewfs/migrations/0002_checksum_state.sql. */
static const char MIGRATION_0002[] =
    "ALTER TABLE objects ADD COLUMN checksum_state BYTEA;";

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
    if (applied_v < 2) {
        vfs_error rc = vfs_exec_simple(s, "BEGIN");
        if (rc != VFS_OK) return rc;
        rc = vfs_exec_simple(s, MIGRATION_0002);
        if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }
        rc = vfs_exec_simple(s,
            "INSERT INTO schema_migrations (version) VALUES (2)");
        if (rc != VFS_OK) { vfs_exec_simple(s, "ROLLBACK"); return rc; }
        rc = vfs_exec_simple(s, "COMMIT");
        if (rc != VFS_OK) return rc;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------
 * Store lifecycle
 * ------------------------------------------------------------------ */

static vfs_error connect_pq(vfs_store *s) {
    const char *ci = s->conninfo[0] ? s->conninfo : NULL;
    PGconn *pg = PQconnectdb(ci ? ci : "");
    if (PQstatus(pg) != CONNECTION_OK) {
        vfs_seterr(s, "PostgreSQL connect failed: %s", PQerrorMessage(pg));
        PQfinish(pg);
        return VFS_ERR_DB;
    }
    s->pg = pg;
    return VFS_OK;
}

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

    rc = connect_pq(&local);
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

vfs_error vfs_check_mappings_bad_objref(vfs_store *s, int64_t *out) {
    if (!s || !out) return VFS_ERR_BADARGS;
    return count_query(s,
        "SELECT count(*) FROM mappings m "
        "WHERE m.object_id IS NOT NULL "
        "  AND NOT EXISTS (SELECT 1 FROM objects o "
        "                  WHERE o.object_id = m.object_id)",
        out);
}

vfs_error vfs_check_mappings_dir_invariant(vfs_store *s, int64_t *out) {
    if (!s || !out) return VFS_ERR_BADARGS;
    /* Both halves of the CHECK constraint -- belt and braces. */
    return count_query(s,
        "SELECT count(*) FROM mappings "
        "WHERE (entry_kind = 'dir')  <> (object_id IS NULL)",
        out);
}

vfs_error vfs_count_rows(vfs_store *s, const char *table_id, int64_t *out) {
    if (!s || !out || !table_id) return VFS_ERR_BADARGS;
    /* table_id is an enum-like constant; whitelist to avoid SQL injection
     * via the FROM clause. */
    static const char *const allowed[] = {
        "objects", "views", "mappings", "attributes", "tags", NULL,
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
