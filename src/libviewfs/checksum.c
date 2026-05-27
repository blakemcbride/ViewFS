#define _POSIX_C_SOURCE 200809L

/* The OpenSSL EVP API doesn't expose intermediate state for serialization.
 * The dedicated SHA256_* API does — SHA256_CTX is a public, fixed-layout
 * struct we can memcpy in and out of BYTEA. OpenSSL 3.x marks these
 * symbols deprecated in favor of EVP, but they remain part of the public
 * ABI; silence the warning only for this file. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "internal.h"

_Static_assert(sizeof(SHA256_CTX) == VFS_SHA256_STATE_LEN,
               "VFS_SHA256_STATE_LEN must match sizeof(SHA256_CTX)");

static const char HEX[] = "0123456789abcdef";

static void raw_to_hex(const unsigned char raw[32], char hex_out[65]) {
    for (int i = 0; i < 32; i++) {
        hex_out[2 * i]     = HEX[(raw[i] >> 4) & 0xF];
        hex_out[2 * i + 1] = HEX[raw[i] & 0xF];
    }
    hex_out[64] = '\0';
}

vfs_error vfs_sha256_hex_path(const char *path, char hex_out[65]) {
    if (!path || !hex_out) return VFS_ERR_BADARGS;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return VFS_ERR_IO;

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    unsigned char buf[64 * 1024];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return VFS_ERR_IO;
        }
        SHA256_Update(&ctx, buf, (size_t)r);
    }
    close(fd);

    unsigned char raw[32];
    SHA256_Final(raw, &ctx);
    raw_to_hex(raw, hex_out);
    return VFS_OK;
}

/* Streaming helpers used by vfs_content_import_host and by the FUSE
 * daemon's per-fd append tracking. The opaque `ctx` member is treated
 * as a SHA256_CTX. */

vfs_error vfs_sha256_stream_init(vfs_sha256_stream *st) {
    if (!st) return VFS_ERR_BADARGS;
    st->ctx = malloc(sizeof(SHA256_CTX));
    if (!st->ctx) return VFS_ERR_NOMEM;
    SHA256_Init((SHA256_CTX *)st->ctx);
    return VFS_OK;
}

vfs_error vfs_sha256_stream_update(vfs_sha256_stream *st,
                                   const void *buf, size_t n) {
    if (!st || !st->ctx) return VFS_ERR_BADARGS;
    if (n == 0) return VFS_OK;
    SHA256_Update((SHA256_CTX *)st->ctx, buf, n);
    return VFS_OK;
}

/* Snapshot the current state without finalizing — so the caller can
 * later restore and continue updating. `state_out` must be at least
 * VFS_SHA256_STATE_LEN bytes. */
vfs_error vfs_sha256_stream_snapshot(vfs_sha256_stream *st,
                                     void *state_out) {
    if (!st || !st->ctx || !state_out) return VFS_ERR_BADARGS;
    memcpy(state_out, st->ctx, VFS_SHA256_STATE_LEN);
    return VFS_OK;
}

/* Compute the current hex digest by snapshotting + finalizing a copy,
 * leaving the live stream untouched and resumable. */
vfs_error vfs_sha256_stream_peek_hex(vfs_sha256_stream *st,
                                     char hex_out[65]) {
    if (!st || !st->ctx || !hex_out) return VFS_ERR_BADARGS;
    SHA256_CTX copy = *(SHA256_CTX *)st->ctx;
    unsigned char raw[32];
    SHA256_Final(raw, &copy);
    raw_to_hex(raw, hex_out);
    return VFS_OK;
}

vfs_error vfs_sha256_stream_finalize(vfs_sha256_stream *st, char hex_out[65]) {
    if (!st || !st->ctx || !hex_out) return VFS_ERR_BADARGS;
    unsigned char raw[32];
    SHA256_Final(raw, (SHA256_CTX *)st->ctx);
    free(st->ctx);
    st->ctx = NULL;
    raw_to_hex(raw, hex_out);
    return VFS_OK;
}

void vfs_sha256_stream_abort(vfs_sha256_stream *st) {
    if (!st || !st->ctx) return;
    free(st->ctx);
    st->ctx = NULL;
}

/* Restore a previously-snapshotted state so writes can resume. Returns
 * BADARGS if state_len doesn't match the known SHA-256 ctx size. */
vfs_error vfs_sha256_stream_restore(vfs_sha256_stream *st,
                                    const void *state_in, size_t state_len) {
    if (!st || !state_in) return VFS_ERR_BADARGS;
    if (state_len != VFS_SHA256_STATE_LEN) return VFS_ERR_BADARGS;
    if (!st->ctx) {
        st->ctx = malloc(sizeof(SHA256_CTX));
        if (!st->ctx) return VFS_ERR_NOMEM;
    }
    memcpy(st->ctx, state_in, VFS_SHA256_STATE_LEN);
    return VFS_OK;
}

#pragma GCC diagnostic pop
