#include <stdio.h>
#include <string.h>

#include "internal.h"

/* NOTIFY channel name used by the daemon's LISTEN thread. */
static const char CHANNEL[] = "viewfs_change";

void vfs_notify_path(vfs_store *s, const char *view, const char *parent_path) {
    if (!s || !view) return;
    char payload[VFS_PATH_MAX + 256];
    int n = snprintf(payload, sizeof payload, "%s\t%s",
                     view, parent_path ? parent_path : "");
    if (n < 0 || (size_t)n >= sizeof payload) return;

    const char *params[2] = { CHANNEL, payload };
    PGresult *r = PQexecParams(s->pg,
        "SELECT pg_notify($1, $2)",
        2, NULL, params, NULL, NULL, 0);
    /* NOTIFY failures are non-fatal — they don't roll back the mutation
     * that preceded them. The daemon's 2-second attr/entry timeout is a
     * safety net for missed notifications. */
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        /* Stash in last_error but don't bubble up. */
        vfs_seterr_pq(s, r);
    }
    PQclear(r);
}
