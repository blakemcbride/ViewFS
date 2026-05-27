#include <string.h>

#include "internal.h"

/* Canonicalize an absolute view path.
 *
 * Rules:
 *   - input must begin with '/'
 *   - '\0' is rejected (impossible in a C string but checked at input)
 *   - components containing '/' are impossible after splitting
 *   - '.' segments are dropped
 *   - '..' pops the previous segment; popping past root is an error
 *   - empty segments (from '//') are dropped
 *   - components > VFS_NAME_MAX bytes are rejected
 *   - any control character (< 0x20) other than the canonical separators
 *     is rejected
 */
vfs_error vfs_path_canonicalize(const char *input, vfs_canon_path *out) {
    if (!input || !out) return VFS_ERR_BADARGS;

    memset(out, 0, sizeof *out);

    if (input[0] != '/') return VFS_ERR_PATH_RELATIVE;

    /* Stack of segment offsets into out->path, growing forward. */
    char  path[VFS_PATH_MAX];
    int   stack[VFS_PATH_MAX / 2];
    int   sp = 0;
    size_t plen = 0;

    const char *p = input + 1;
    while (*p) {
        /* extract next segment up to '/' or end */
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - start);

        if (seglen == 0) {
            /* empty segment from '//', skip */
        } else if (seglen == 1 && start[0] == '.') {
            /* '.' segment, skip */
        } else if (seglen == 2 && start[0] == '.' && start[1] == '.') {
            if (sp == 0) return VFS_ERR_PATH_ESCAPE;
            sp--;
            plen = (size_t)stack[sp];
        } else {
            if (seglen > VFS_NAME_MAX) return VFS_ERR_PATH_BADCHAR;
            for (size_t i = 0; i < seglen; i++) {
                unsigned char c = (unsigned char)start[i];
                if (c == 0) return VFS_ERR_PATH_BADCHAR;
                if (c < 0x20) return VFS_ERR_PATH_BADCHAR;
            }
            /* push current plen onto the stack so '..' can return here */
            if (sp >= (int)(sizeof stack / sizeof stack[0]))
                return VFS_ERR_PATH_BADCHAR;
            stack[sp++] = (int)plen;
            if (plen + 1 + seglen >= sizeof path) return VFS_ERR_PATH_BADCHAR;
            path[plen++] = '/';
            memcpy(path + plen, start, seglen);
            plen += seglen;
        }

        if (*p == '/') p++;
    }

    if (plen == 0) {
        /* root */
        out->path[0] = '/';
        out->path[1] = '\0';
        out->parent[0] = '\0';
        out->name[0]   = '\0';
        out->is_root   = 1;
        return VFS_OK;
    }

    path[plen] = '\0';
    if (plen >= sizeof out->path) return VFS_ERR_PATH_BADCHAR;
    memcpy(out->path, path, plen + 1);

    /* parent + name from the last stack slot */
    int last_off = stack[sp - 1];
    if (last_off == 0) {
        out->parent[0] = '\0';
    } else {
        size_t pl = (size_t)last_off;
        if (pl >= sizeof out->parent) return VFS_ERR_PATH_BADCHAR;
        memcpy(out->parent, path, pl);
        out->parent[pl] = '\0';
    }
    size_t namelen = plen - (size_t)last_off - 1;
    if (namelen > VFS_NAME_MAX) return VFS_ERR_PATH_BADCHAR;
    memcpy(out->name, path + last_off + 1, namelen);
    out->name[namelen] = '\0';
    return VFS_OK;
}
