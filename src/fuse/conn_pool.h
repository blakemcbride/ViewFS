#ifndef VIEWFS_FUSE_CONN_POOL_H
#define VIEWFS_FUSE_CONN_POOL_H

#include <libpq-fe.h>
#include <pthread.h>

#include "viewfs/viewfs.h"

/* Per-worker-thread libpq connection pool.
 *
 * libpq's PGconn is not safe to share across threads. The FUSE daemon
 * therefore allocates one PGconn per worker thread, lazily on first use,
 * stored in thread-local storage and closed by the TLS destructor when
 * the thread exits.
 *
 * The conninfo and schema strings are copied at init() time. The pool
 * may be safely shared across threads.
 */
typedef struct {
    char           conninfo[1024];
    char           schema[64];
    pthread_key_t  key;
    pthread_once_t once;
} conn_pool;

/* Initialize a pool from a viewfs store's conninfo/schema. Returns VFS_OK
 * on success. */
vfs_error conn_pool_init(conn_pool *p, const char *conninfo,
                         const char *schema);

/* Return the calling thread's PGconn, opening it lazily. Returns NULL on
 * failure (libpq error is then visible via PQerrorMessage on the connection
 * that was attempted -- but the failed connection is discarded). */
PGconn *conn_pool_get(conn_pool *p);

/* Destroy the pool. Closes the calling thread's connection; other threads
 * close theirs via the TLS destructor. */
void conn_pool_destroy(conn_pool *p);

#endif
