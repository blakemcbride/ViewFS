#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

#include "internal.h"

static const char HEX[] = "0123456789abcdef";

vfs_error vfs_object_id_generate(vfs_object_id *out) {
    if (!out) return VFS_ERR_BADARGS;
    unsigned char raw[16];
    size_t got = 0;
    while (got < sizeof raw) {
        ssize_t n = getrandom(raw + got, sizeof raw - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return VFS_ERR_IO;
        }
        got += (size_t)n;
    }
    for (size_t i = 0; i < sizeof raw; i++) {
        out->hex[2 * i]     = HEX[(raw[i] >> 4) & 0xF];
        out->hex[2 * i + 1] = HEX[raw[i]        & 0xF];
    }
    out->hex[VFS_OID_HEX_LEN] = '\0';
    return VFS_OK;
}

int vfs_object_id_valid(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n != VFS_OID_HEX_LEN) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}
