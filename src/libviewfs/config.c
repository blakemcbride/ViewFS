/* Minimal TOML-subset reader/writer for ViewFS config.toml.
 *
 * Accepted syntax:
 *   # comment    or    ; comment
 *   key = 123                     (integer)
 *   key = "string with \"esc\""   (escapes: \" \\)
 *   [section]
 *
 * Unsupported (intentionally): arrays, multiline strings, inline tables,
 * floats, dates, booleans, dotted keys, hex/oct/binary, multiline values.
 *
 * The store only needs four keys, so this parser exists only to read and
 * write that small surface. */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

static char *str_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

/* Parse a TOML scalar value at *p into out (size outsz). On success,
 * advances *p past the value and any trailing whitespace/comment, and
 * returns VFS_OK. */
static vfs_error parse_value(const char **p, char *out, size_t outsz) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '"') {
        s++;
        size_t o = 0;
        while (*s && *s != '"') {
            if (*s == '\\' && (s[1] == '"' || s[1] == '\\')) {
                if (o + 1 >= outsz) return VFS_ERR_CONFIG;
                out[o++] = s[1];
                s += 2;
            } else {
                if (o + 1 >= outsz) return VFS_ERR_CONFIG;
                out[o++] = *s++;
            }
        }
        if (*s != '"') return VFS_ERR_CONFIG;
        out[o] = '\0';
        s++;
    } else {
        /* bare integer or word; stop at whitespace or comment */
        size_t o = 0;
        while (*s && *s != '#' && *s != ';' && *s != '\n' &&
               !isspace((unsigned char)*s)) {
            if (o + 1 >= outsz) return VFS_ERR_CONFIG;
            out[o++] = *s++;
        }
        out[o] = '\0';
    }
    *p = s;
    return VFS_OK;
}

vfs_error vfs_store_load_config(vfs_store *s) {
    char path[VFS_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", s->store_path, VFS_CONFIG_FILE);
    if (n < 0 || (size_t)n >= sizeof path) return VFS_ERR_CONFIG;

    FILE *f = fopen(path, "r");
    if (!f) {
        vfs_seterr(s, "open %s: %s", path, strerror(errno));
        return VFS_ERR_CONFIG;
    }

    char line[2048];
    char section[64] = "";
    vfs_error rc = VFS_OK;

    while (fgets(line, sizeof line, f)) {
        char *t = str_trim(line);
        if (!*t || *t == '#' || *t == ';') continue;
        if (*t == '[') {
            char *e = strchr(t, ']');
            if (!e) { rc = VFS_ERR_CONFIG; break; }
            *e = '\0';
            char *name = str_trim(t + 1);
            if (strlen(name) >= sizeof section) { rc = VFS_ERR_CONFIG; break; }
            snprintf(section, sizeof section, "%s", name);
            continue;
        }
        char *eq = strchr(t, '=');
        if (!eq) { rc = VFS_ERR_CONFIG; break; }
        *eq = '\0';
        char *key = str_trim(t);
        const char *v = eq + 1;
        char value[1024];
        rc = parse_value(&v, value, sizeof value);
        if (rc != VFS_OK) break;

        if (section[0] == '\0') {
            if (!strcmp(key, "store_version")) {
                s->store_version = atoi(value);
            } else if (!strcmp(key, "shard_depth")) {
                s->shard_depth = atoi(value);
            }
        } else if (!strcmp(section, "postgres")) {
            if (!strcmp(key, "conninfo")) {
                snprintf(s->conninfo, sizeof s->conninfo, "%s", value);
            } else if (!strcmp(key, "schema")) {
                if (!vfs_ident_ok(value) || strlen(value) >= sizeof s->schema) {
                    vfs_seterr(s, "invalid postgres.schema '%s'", value);
                    rc = VFS_ERR_CONFIG;
                    break;
                }
                memcpy(s->schema, value, strlen(value) + 1);
            }
        }
    }

    fclose(f);
    if (rc != VFS_OK) return rc;

    if (s->store_version == 0) s->store_version = 1;
    if (s->shard_depth   == 0) s->shard_depth   = VFS_SHARD_DEPTH;
    if (s->schema[0] == '\0')
        snprintf(s->schema, sizeof s->schema, "%s", VFS_DEFAULT_SCHEMA);
    return VFS_OK;
}

static void quote_string(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    if (o + 1 < outsz) out[o++] = '"';
    for (const char *p = in; *p && o + 2 < outsz; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 < outsz) {
                out[o++] = '\\';
                out[o++] = *p;
            }
        } else {
            out[o++] = *p;
        }
    }
    if (o + 1 < outsz) out[o++] = '"';
    if (o < outsz) out[o] = '\0';
    else out[outsz - 1] = '\0';
}

vfs_error vfs_store_save_config(const vfs_store *s) {
    char path[VFS_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", s->store_path, VFS_CONFIG_FILE);
    if (n < 0 || (size_t)n >= sizeof path) return VFS_ERR_CONFIG;

    char tmp[VFS_PATH_MAX];
    n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp) return VFS_ERR_CONFIG;

    FILE *f = fopen(tmp, "w");
    if (!f) return VFS_ERR_IO;

    char qconn[1200], qschema[128];
    quote_string(s->conninfo, qconn,   sizeof qconn);
    quote_string(s->schema,   qschema, sizeof qschema);

    fprintf(f,
        "# ViewFS backing-store configuration. Managed by `viewfs init`.\n"
        "store_version = %d\n"
        "shard_depth   = %d\n"
        "\n"
        "[postgres]\n"
        "conninfo = %s\n"
        "schema   = %s\n",
        s->store_version, s->shard_depth, qconn, qschema);

    if (fflush(f) != 0 || fclose(f) != 0) return VFS_ERR_IO;

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return VFS_ERR_IO;
    }
    return VFS_OK;
}
