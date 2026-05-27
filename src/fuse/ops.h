#ifndef VIEWFS_FUSE_OPS_H
#define VIEWFS_FUSE_OPS_H

#define FUSE_USE_VERSION 31
#include <fuse.h>

#include "conn_pool.h"
#include "viewfs/viewfs.h"

/* Daemon-wide context shared across FUSE worker threads.
 *
 * All fields are read-only after main() finishes setup, except for the
 * conn_pool which is its own synchronization. */
typedef struct {
    vfs_store   *store;        /* used for content-path helpers + store_path */
    conn_pool   *pool;
    char         view_name[128];
    int          read_only;
    int          verbose;
    struct fuse *fuse_handle;  /* captured in op_init via fuse_get_context() */
} viewfs_ctx;

extern viewfs_ctx CTX;

extern const struct fuse_operations viewfs_oper;

#endif
