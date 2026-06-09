#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

const char *cli_take_flag(int *argc, char **argv, const char *flag, int boolean) {
    size_t flen = strlen(flag);
    for (int i = 1; i < *argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            if (boolean) {
                /* remove argv[i] */
                for (int j = i; j < *argc - 1; j++) argv[j] = argv[j+1];
                (*argc)--;
                return "1";
            }
            if (i + 1 >= *argc) return NULL;
            const char *val = argv[i + 1];
            for (int j = i; j < *argc - 2; j++) argv[j] = argv[j+2];
            (*argc) -= 2;
            return val;
        }
        if (!boolean && strncmp(argv[i], flag, flen) == 0 && argv[i][flen] == '=') {
            const char *val = argv[i] + flen + 1;
            for (int j = i; j < *argc - 1; j++) argv[j] = argv[j+1];
            (*argc)--;
            return val;
        }
    }
    return NULL;
}

vfs_store *cli_open_store(int *argc, char **argv, int *exit_rc) {
    const char *store_path = cli_take_flag(argc, argv, "--store", 0);
    if (!store_path) store_path = getenv("VIEWFS_STORE");
    if (!store_path || !*store_path) {
        fprintf(stderr,
            "viewfs: store path required (pass --store PATH or set VIEWFS_STORE)\n");
        *exit_rc = 2;
        return NULL;
    }
    vfs_store *s = NULL;
    vfs_error rc = vfs_store_open(store_path, &s);
    if (rc != VFS_OK) {
        /* error message already printed by vfs_store_open */
        *exit_rc = 1;
        return NULL;
    }
    return s;
}

int cli_resolve_object(vfs_store *s, const char *arg, vfs_object_id *id) {
    vfs_error rc = vfs_object_resolve(s, arg, id);
    if (rc == VFS_OK) return 0;
    if (rc == VFS_ERR_NOTFOUND)
        fprintf(stderr, "viewfs: no object matching '%s'\n", arg);
    else if (rc == VFS_ERR_AMBIGUOUS)
        fprintf(stderr, "viewfs: object prefix '%s' is ambiguous\n", arg);
    else
        fprintf(stderr, "viewfs: %s (%s)\n",
                vfs_error_str(rc), vfs_store_last_error(s));
    return 1;
}

/* Decode /proc/mounts octal escapes (\040 space, \011 tab, \012 nl, \134 \). */
static void unescape_mount(const char *in, char *out, size_t osz) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < osz; ) {
        if (in[i] == '\\' &&
            in[i+1] >= '0' && in[i+1] <= '7' &&
            in[i+2] >= '0' && in[i+2] <= '7' &&
            in[i+3] >= '0' && in[i+3] <= '7') {
            out[o++] = (char)((in[i+1]-'0')*64 + (in[i+2]-'0')*8 + (in[i+3]-'0'));
            i += 4;
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

/* True if the comma-separated mount-option list contains a bare "ro". */
static int opts_read_only(const char *opts) {
    for (const char *p = opts; *p; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && !strncmp(p, "ro", 2)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int cli_foreach_viewfs_mount(
    void (*cb)(const char *view, const char *mountpoint, int read_only, void *ud),
    void *ud) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return -1;
    char line[8192];
    int count = 0;
    while (fgets(line, sizeof line, f)) {
        /* fields: <device> <mountpoint> <fstype> <options> ... */
        char *dev = line;
        char *sp1 = strchr(dev, ' ');           if (!sp1) continue; *sp1 = '\0';
        if (strncmp(dev, "viewfs:", 7) != 0) continue;
        char *mnt_raw = sp1 + 1;
        char *sp2 = strchr(mnt_raw, ' ');        if (!sp2) continue; *sp2 = '\0';
        char *fstype = sp2 + 1;
        char *sp3 = strchr(fstype, ' ');         if (!sp3) continue; *sp3 = '\0';
        char *opts = sp3 + 1;
        char *sp4 = strchr(opts, ' ');           if (sp4) *sp4 = '\0';

        char mnt[VFS_PATH_MAX];
        unescape_mount(mnt_raw, mnt, sizeof mnt);
        cb(dev + 7, mnt, opts_read_only(opts), ud);
        count++;
    }
    fclose(f);
    return count;
}

int cli_view_dir_from_cwd(char *view_out, size_t vsz, char *dir_out, size_t dsz) {
    char cwd[VFS_PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return -1;

    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return -1;

    char line[8192];
    char best_mnt[VFS_PATH_MAX] = "";
    char best_view[128]         = "";
    size_t best_len = 0;

    while (fgets(line, sizeof line, f)) {
        /* "<device> <mountpoint> <fstype> ..." — first two whitespace fields. */
        char *sp1 = strchr(line, ' ');
        if (!sp1) continue;
        *sp1 = '\0';
        const char *dev = line;
        if (strncmp(dev, "viewfs:", 7) != 0) continue;
        char *mnt_raw = sp1 + 1;
        char *sp2 = strchr(mnt_raw, ' ');
        if (!sp2) continue;
        *sp2 = '\0';

        char mnt[VFS_PATH_MAX];
        unescape_mount(mnt_raw, mnt, sizeof mnt);
        size_t mlen = strlen(mnt);
        if (mlen == 0) continue;

        int under = !strcmp(cwd, mnt) ||
            (!strncmp(cwd, mnt, mlen) && (mnt[mlen-1] == '/' || cwd[mlen] == '/'));
        if (under && mlen >= best_len) {
            best_len = mlen;
            snprintf(best_mnt,  sizeof best_mnt,  "%s", mnt);
            snprintf(best_view, sizeof best_view, "%.*s",
                     (int)sizeof best_view - 1, dev + 7);
        }
    }
    fclose(f);
    if (best_view[0] == '\0') return -1;

    const char *rel = cwd + strlen(best_mnt);   /* "" or "/sub/dir" */
    vfs_canon_path cp;
    if (vfs_path_canonicalize(*rel ? rel : "/", &cp) != VFS_OK) return -1;
    snprintf(view_out, vsz, "%s", best_view);
    snprintf(dir_out,  dsz, "%s", cp.path);
    return 0;
}

/* out = base + "/" + leaf (no doubled slash). Returns -1 if it won't fit. */
static int join_path(char *out, size_t osz, const char *base, const char *leaf) {
    size_t bl = strlen(base), ll = strlen(leaf);
    int slash = (bl == 0 || base[bl - 1] != '/') ? 1 : 0;
    if (bl + (size_t)slash + ll + 1 > osz) return -1;
    memcpy(out, base, bl);
    size_t o = bl;
    if (slash) out[o++] = '/';
    memcpy(out + o, leaf, ll);
    out[o + ll] = '\0';
    return 0;
}

int cli_resolve_dir(int n, const char *a0, const char *a1,
                    char *view_out, size_t vsz, char *dir_out, size_t dsz) {
    vfs_canon_path cp;
    if (n == 2) {
        if (vfs_path_canonicalize(a1, &cp) != VFS_OK) {
            fprintf(stderr, "viewfs: invalid directory path '%s'\n", a1);
            return -1;
        }
        snprintf(view_out, vsz, "%s", a0);
        snprintf(dir_out,  dsz, "%s", cp.path);
        return 0;
    }

    char cwd_view[128], cwd_dir[VFS_PATH_MAX];
    if (cli_view_dir_from_cwd(cwd_view, sizeof cwd_view,
                              cwd_dir, sizeof cwd_dir) != 0) {
        fprintf(stderr, "viewfs: not inside a mounted ViewFS view; "
                        "give VIEW and DIR explicitly\n");
        return -1;
    }
    snprintf(view_out, vsz, "%s", cwd_view);

    if (n == 0) {                       /* operate on the current directory */
        snprintf(dir_out, dsz, "%s", cwd_dir);
        return 0;
    }

    /* n == 1: resolve a0 like a shell path against the current in-view dir.
     * Absolute (leading '/') is taken as-is; otherwise joined onto cwd_dir. */
    char joined[VFS_PATH_MAX];
    int too_long = (a0[0] == '/')
        ? (strlen(a0) + 1 > sizeof joined
              ? -1 : (memcpy(joined, a0, strlen(a0) + 1), 0))
        : join_path(joined, sizeof joined, cwd_dir, a0);
    if (too_long != 0) {
        fprintf(stderr, "viewfs: directory path too long\n");
        return -1;
    }
    if (vfs_path_canonicalize(joined, &cp) != VFS_OK) {
        fprintf(stderr, "viewfs: invalid directory path '%s'\n", a0);
        return -1;
    }
    snprintf(dir_out, dsz, "%s", cp.path);
    return 0;
}

int cli_is_help_request(const char *arg) {
    if (!arg) return 0;
    return !strcmp(arg, "--help") || !strcmp(arg, "-h") || !strcmp(arg, "help");
}

int cli_perror(vfs_store *s, vfs_error e, const char *context) {
    const char *msg = vfs_error_str(e);
    const char *detail = s ? vfs_store_last_error(s) : NULL;
    if (context) {
        fprintf(stderr, "viewfs: %s: %s", context, msg);
    } else {
        fprintf(stderr, "viewfs: %s", msg);
    }
    if (detail && strcmp(detail, "(no error)") != 0 && *detail) {
        fprintf(stderr, " (%s)", detail);
    }
    fputc('\n', stderr);
    return 1;
}
