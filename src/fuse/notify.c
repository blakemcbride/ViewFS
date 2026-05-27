#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "notify.h"

static pthread_t       NTHR;
static atomic_int      STOP = 0;
static int             RUNNING = 0;
static PGconn         *NCONN = NULL;
static viewfs_ctx     *NCTX = NULL;

/* Connect a dedicated PGconn, SET search_path, and LISTEN. */
static PGconn *open_listen_conn(viewfs_ctx *ctx) {
    const char *ci = ctx->pool->conninfo[0] ? ctx->pool->conninfo : NULL;
    PGconn *pg = PQconnectdb(ci ? ci : "");
    if (PQstatus(pg) != CONNECTION_OK) {
        fprintf(stderr, "viewfs-fuse: notify PQconnectdb failed: %s",
                PQerrorMessage(pg));
        PQfinish(pg);
        return NULL;
    }
    char sql[256];
    snprintf(sql, sizeof sql, "SET search_path TO \"%s\"", ctx->pool->schema);
    PGresult *r = PQexec(pg, sql);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "viewfs-fuse: notify SET search_path: %s",
                PQresultErrorMessage(r));
        PQclear(r); PQfinish(pg); return NULL;
    }
    PQclear(r);
    r = PQexec(pg, "LISTEN viewfs_change");
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "viewfs-fuse: LISTEN failed: %s",
                PQresultErrorMessage(r));
        PQclear(r); PQfinish(pg); return NULL;
    }
    PQclear(r);
    return pg;
}

/* Payload format: "<view>\t<parent_path>". Parse and act. */
static void handle_payload(viewfs_ctx *ctx, char *payload) {
    char *tab = strchr(payload, '\t');
    if (!tab) return;
    *tab = '\0';
    const char *view = payload;
    const char *parent = tab + 1;

    if (strcmp(view, ctx->view_name) != 0) return;

    const char *to_invalidate = parent[0] ? parent : "/";
    int rc = fuse_invalidate_path(ctx->fuse_handle, to_invalidate);
    if (ctx->verbose) {
        fprintf(stderr, "viewfs-fuse[%s]: invalidate %s -> %d\n",
                ctx->view_name, to_invalidate, rc);
    }
}

static void *notify_loop(void *arg) {
    viewfs_ctx *ctx = arg;
    while (!atomic_load(&STOP)) {
        int sock = PQsocket(NCONN);
        if (sock < 0) break;

        fd_set rds;
        FD_ZERO(&rds);
        FD_SET(sock, &rds);
        struct timeval tv = { 1, 0 };  /* 1 s poll for shutdown */
        int sret = select(sock + 1, &rds, NULL, NULL, &tv);
        if (sret < 0 && errno != EINTR) break;

        if (!PQconsumeInput(NCONN)) {
            fprintf(stderr, "viewfs-fuse: notify PQconsumeInput: %s",
                    PQerrorMessage(NCONN));
            break;
        }
        PGnotify *n;
        while ((n = PQnotifies(NCONN)) != NULL) {
            if (n->extra) {
                /* PQnotifies guarantees a NUL-terminated string we can mutate */
                handle_payload(ctx, n->extra);
            }
            PQfreemem(n);
        }
    }
    return NULL;
}

int notify_thread_start(viewfs_ctx *ctx) {
    if (RUNNING) return 0;
    NCONN = open_listen_conn(ctx);
    if (!NCONN) return -1;
    NCTX = ctx;
    atomic_store(&STOP, 0);
    if (pthread_create(&NTHR, NULL, notify_loop, ctx) != 0) {
        PQfinish(NCONN); NCONN = NULL;
        return -1;
    }
    RUNNING = 1;
    return 0;
}

void notify_thread_stop(void) {
    if (!RUNNING) return;
    atomic_store(&STOP, 1);
    pthread_join(NTHR, NULL);
    if (NCONN) { PQfinish(NCONN); NCONN = NULL; }
    NCTX = NULL;
    RUNNING = 0;
}
