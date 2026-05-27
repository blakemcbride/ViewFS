#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conn_pool.h"

/* The pool itself is referenced through a single global so the TLS
 * destructor doesn't need a back-pointer. The daemon only ever owns one
 * pool at a time. */
static conn_pool *active_pool = NULL;

static void tls_destructor(void *value) {
    PGconn *c = value;
    if (c) PQfinish(c);
}

vfs_error conn_pool_init(conn_pool *p, const char *conninfo, const char *schema) {
    if (!p || !schema) return VFS_ERR_BADARGS;
    memset(p, 0, sizeof *p);
    snprintf(p->conninfo, sizeof p->conninfo, "%s", conninfo ? conninfo : "");
    snprintf(p->schema,   sizeof p->schema,   "%s", schema);
    if (pthread_key_create(&p->key, tls_destructor) != 0) return VFS_ERR_INTERNAL;
    p->once = (pthread_once_t)PTHREAD_ONCE_INIT;
    active_pool = p;
    return VFS_OK;
}

PGconn *conn_pool_get(conn_pool *p) {
    if (!p) return NULL;
    PGconn *c = pthread_getspecific(p->key);
    if (c) return c;

    const char *ci = p->conninfo[0] ? p->conninfo : NULL;
    PGconn *nc = PQconnectdb(ci ? ci : "");
    if (PQstatus(nc) != CONNECTION_OK) {
        fprintf(stderr, "viewfs-fuse: libpq connect failed: %s",
                PQerrorMessage(nc));
        PQfinish(nc);
        return NULL;
    }

    char sql[256];
    snprintf(sql, sizeof sql, "SET search_path TO \"%s\"", p->schema);
    PGresult *r = PQexec(nc, sql);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "viewfs-fuse: SET search_path failed: %s",
                PQresultErrorMessage(r));
        PQclear(r);
        PQfinish(nc);
        return NULL;
    }
    PQclear(r);

    if (pthread_setspecific(p->key, nc) != 0) {
        PQfinish(nc);
        return NULL;
    }
    return nc;
}

void conn_pool_destroy(conn_pool *p) {
    if (!p) return;
    PGconn *c = pthread_getspecific(p->key);
    if (c) {
        PQfinish(c);
        pthread_setspecific(p->key, NULL);
    }
    pthread_key_delete(p->key);
    if (active_pool == p) active_pool = NULL;
}
