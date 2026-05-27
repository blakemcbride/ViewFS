#ifndef VIEWFS_FUSE_NOTIFY_H
#define VIEWFS_FUSE_NOTIFY_H

#include "ops.h"

/* Spawn the background thread that runs LISTEN viewfs_change on a
 * dedicated PGconn and calls fuse_invalidate_path() for paths affected
 * by notifications matching CTX.view_name.
 *
 * Safe to call once from op_init(). Returns 0 on success. */
int notify_thread_start(viewfs_ctx *ctx);

/* Stop the notify thread and close its connection. Called from op_destroy. */
void notify_thread_stop(void);

#endif
