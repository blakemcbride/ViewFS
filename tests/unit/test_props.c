/* DB-backed unit tests for the property-driven membership model (Phase R0).
 *
 * Exercises: effective property sets (independent vs flowed), superset
 * membership (multi-value AND, empty-set-matches-all), name resolution and
 * ambiguity, orphan listing, and the consistency-check helpers.
 *
 * Requires a reachable PostgreSQL. Connection comes from $VIEWFS_TEST_PG
 * (default: host=/var/run/postgresql user=postgres dbname=viewfs). If the
 * server is unreachable the test SKIPS (exit 0) so `make unit-test` still
 * passes in DB-less environments.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libpq-fe.h>
#include "viewfs/viewfs.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   " __VA_ARGS__); putchar('\n'); } \
    else { failures++; fprintf(stderr, "FAIL " __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* ---- helpers -------------------------------------------------------- */

static int64_t g_count;
static void count_cb(const vfs_object_info *info, void *ud) {
    (void)info; (void)ud; g_count++;
}
static int64_t count_members(vfs_store *s, const char *view, const char *dir) {
    g_count = 0;
    vfs_error rc = vfs_dir_members(s, view, dir, count_cb, NULL);
    if (rc != VFS_OK) { fprintf(stderr, "  members(%s) rc=%s: %s\n",
                                dir, vfs_error_str(rc), vfs_store_last_error(s)); return -1; }
    return g_count;
}
static void prop_count_cb(const vfs_dirprop_row *row, void *ud) {
    (void)row; (*(int *)ud)++;
}
static int effective_prop_count(vfs_store *s, const char *view, const char *dir) {
    int n = 0;
    vfs_dir_prop_list(s, view, dir, 1, prop_count_cb, &n);
    return n;
}

static vfs_object_id mkobj(vfs_store *s, const char *name) {
    vfs_object_id id;
    vfs_error rc = vfs_object_create_file(s, name, 0644, 0, -1, -1,
                                          NULL, NULL, 0, NULL, 0, &id);
    if (rc != VFS_OK) { fprintf(stderr, "mkobj(%s) failed: %s\n",
                                name, vfs_store_last_error(s)); exit(2); }
    return id;
}

int main(void) {
    const char *ci = getenv("VIEWFS_TEST_PG");
    if (!ci || !*ci) ci = "host=/var/run/postgresql user=postgres dbname=viewfs";

    /* Probe connectivity; skip cleanly if unreachable. */
    PGconn *probe = PQconnectdb(ci);
    if (PQstatus(probe) != CONNECTION_OK) {
        fprintf(stderr, "SKIP test_props: cannot reach Postgres (%s)\n", ci);
        PQfinish(probe);
        return 0;
    }
    PQfinish(probe);

    char schema[64];
    snprintf(schema, sizeof schema, "viewfs_test_props_%d", (int)getpid());
    char tmpl[] = "/tmp/viewfs_props_XXXXXX";
    char *store = mkdtemp(tmpl);
    if (!store) { perror("mkdtemp"); return 2; }

    vfs_error rc = vfs_store_create(store, ci, schema, 0);
    CHECK(rc == VFS_OK, "store_create");
    if (rc != VFS_OK) return 2;

    vfs_store *s = NULL;
    rc = vfs_store_open(store, &s);
    CHECK(rc == VFS_OK && s, "store_open");
    if (rc != VFS_OK) return 2;

    CHECK(vfs_view_create(s, "v", "test view") == VFS_OK, "view_create (root auto-made)");

    /* Objects with names + properties. */
    vfs_object_id a = mkobj(s, "a.txt");   /* author=blake, year=2024 */
    vfs_object_id b = mkobj(s, "b.txt");   /* author=blake (+ jane) */
    vfs_object_id c = mkobj(s, "c.txt");   /* author=jane,  year=2024 */
    vfs_object_id d = mkobj(s, "d.txt");   /* (no properties) */
    vfs_object_id e = mkobj(s, "a.txt");   /* author=blake -> name clash with a */

    vfs_object_prop_add(s, &a, "author", "blake");
    vfs_object_prop_add(s, &a, "year",   "2024");
    vfs_object_prop_add(s, &b, "author", "blake");
    vfs_object_prop_add(s, &b, "author", "jane");   /* multi-value key */
    vfs_object_prop_add(s, &c, "author", "jane");
    vfs_object_prop_add(s, &c, "year",   "2024");
    vfs_object_prop_add(s, &e, "author", "blake");
    (void)d;

    /* ---- Empty-set root matches all 5 objects ---- */
    CHECK(count_members(s, "v", "/") == 5, "root (empty filter) matches all 5");

    /* ---- Directory filters ---- */
    CHECK(vfs_dir_create(s, "v", "/blake", 0755) == VFS_OK, "mkdir /blake");
    CHECK(vfs_dir_create(s, "v", "/blake/2024", 0755) == VFS_OK, "mkdir /blake/2024 (auto-parents)");

    /* Fresh, empty-prop directory also matches everything. */
    CHECK(count_members(s, "v", "/blake") == 5, "empty /blake matches all 5");

    vfs_dir_prop_add(s, "v", "/blake", "author", "blake", 0 /*no flow*/);
    CHECK(count_members(s, "v", "/blake") == 3, "/blake[author=blake] -> a,b,e");

    vfs_dir_prop_add(s, "v", "/blake/2024", "year", "2024", 0 /*no flow*/);
    /* Independent: /blake/2024 requires only year=2024, NOT author. -> a,c */
    CHECK(count_members(s, "v", "/blake/2024") == 2, "independent: /blake/2024[year=2024] -> a,c");
    CHECK(effective_prop_count(s, "v", "/blake/2024") == 1, "effective set is own-only (1 pair)");

    /* ---- Flow: mark /blake author=blake as flowing down ---- */
    CHECK(vfs_dir_prop_set(s, "v", "/blake", "author", "blake", 1 /*flow*/) == VFS_OK,
          "set /blake author=blake flow=true");
    CHECK(effective_prop_count(s, "v", "/blake/2024") == 2, "effective now 2 (year + flowed author)");
    /* Now /blake/2024 requires year=2024 AND author=blake -> only a */
    CHECK(count_members(s, "v", "/blake/2024") == 1, "flowed: /blake/2024 -> a only");

    /* ---- Multi-value AND in a directory filter ---- */
    CHECK(vfs_dir_create(s, "v", "/both", 0755) == VFS_OK, "mkdir /both");
    vfs_dir_prop_add(s, "v", "/both", "author", "blake", 0);
    vfs_dir_prop_add(s, "v", "/both", "author", "jane", 0);
    /* requires author=blake AND author=jane -> only b (has both) */
    CHECK(count_members(s, "v", "/both") == 1, "/both[author in {blake,jane}] -> b only");

    /* ---- Name resolution + ambiguity ---- */
    vfs_object_id got;
    CHECK(vfs_dir_member_by_name(s, "v", "/both", "b.txt", &got) == VFS_OK
          && !strcmp(got.hex, b.hex), "member_by_name /both b.txt -> b");
    /* a.txt and e.txt both named "a.txt" and both match /blake -> ambiguous */
    CHECK(vfs_dir_member_by_name(s, "v", "/blake", "a.txt", &got) == VFS_ERR_AMBIGUOUS,
          "member_by_name ambiguous name in /blake");
    CHECK(vfs_dir_member_by_name(s, "v", "/blake", "nope.txt", &got) == VFS_ERR_NOTFOUND,
          "member_by_name missing -> NOTFOUND");

    /* ---- Orphans = objects with no properties (just d) ---- */
    g_count = 0;
    vfs_object_list_orphans(s, count_cb, NULL);
    CHECK(g_count == 1, "exactly 1 propertyless object (d) is an orphan");

    /* ---- rmdir semantics ---- */
    CHECK(vfs_dir_remove(s, "v", "/blake/2024") == VFS_ERR_NOTEMPTY,
          "rmdir non-empty (matches a) -> NOTEMPTY");
    CHECK(vfs_dir_create(s, "v", "/none", 0755) == VFS_OK, "mkdir /none");
    vfs_dir_prop_add(s, "v", "/none", "author", "nobody", 0);
    CHECK(count_members(s, "v", "/none") == 0, "/none matches nothing");
    CHECK(vfs_dir_remove(s, "v", "/none") == VFS_OK, "rmdir empty-membership dir ok");

    /* ---- Consistency checks ---- */
    int64_t bad = -1;
    CHECK(vfs_check_dirs_reachable(s, &bad) == VFS_OK && bad == 0, "dirs reachable (0 broken)");
    CHECK(vfs_check_props_orphans(s, &bad) == VFS_OK && bad == 0, "no orphan prop rows");

    /* ---- Delete object removes it from every directory ---- */
    CHECK(vfs_object_delete(s, &a) == VFS_OK, "object delete a");
    CHECK(count_members(s, "v", "/blake/2024") == 0, "after delete, /blake/2024 empty");

    vfs_store_close(s);

    /* Teardown: drop the test schema. */
    PGconn *pg = PQconnectdb(ci);
    if (PQstatus(pg) == CONNECTION_OK) {
        char sql[128];
        snprintf(sql, sizeof sql, "DROP SCHEMA IF EXISTS \"%s\" CASCADE", schema);
        PQclear(PQexec(pg, sql));
    }
    PQfinish(pg);
    /* best-effort temp dir removal */
    char rmcmd[256];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf '%s'", store);
    if (system(rmcmd) != 0) { /* ignore */ }

    if (failures) { fprintf(stderr, "\n%d failure(s)\n", failures); return 1; }
    printf("\nAll property-model tests passed.\n");
    return 0;
}
