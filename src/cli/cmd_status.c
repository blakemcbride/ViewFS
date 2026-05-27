#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h"

/* Recursively walk a directory, summing regular-file sizes and counts. */
static void walk_dir(const char *dir, int64_t *bytes, int64_t *files) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[VFS_PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int n = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))      walk_dir(path, bytes, files);
        else if (S_ISREG(st.st_mode)) { *bytes += (int64_t)st.st_size; (*files)++; }
    }
    closedir(d);
}

static void human(int64_t bytes, char *buf, size_t bufsz) {
    static const char *const suf[] = { "B", "KiB", "MiB", "GiB", "TiB", NULL };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && suf[i + 1]) { v /= 1024.0; i++; }
    if (i == 0) snprintf(buf, bufsz, "%lld %s", (long long)bytes, suf[i]);
    else        snprintf(buf, bufsz, "%.1f %s",  v, suf[i]);
}

static void orphan_count_cb(const vfs_object_info *o, void *ud) {
    (void)o;
    (*(int64_t *)ud)++;
}

int cmd_status(int argc, char **argv) {
    int rc = 0;
    vfs_store *s = cli_open_store(&argc, argv, &rc);
    if (!s) return rc;

    int     sv = -1;
    int64_t nviews = 0, nobjs = 0, nmaps = 0, norph = 0;
    int64_t content_bytes = 0, content_files = 0;

    vfs_schema_version(s, &sv);
    vfs_count_rows(s, "views",    &nviews);
    vfs_count_rows(s, "objects",  &nobjs);
    vfs_count_rows(s, "mappings", &nmaps);
    vfs_object_list_orphans(s, orphan_count_cb, &norph);

    char obj_dir[VFS_PATH_MAX];
    snprintf(obj_dir, sizeof obj_dir, "%s/objects", vfs_store_path(s));
    walk_dir(obj_dir, &content_bytes, &content_files);

    char size_human[32];
    human(content_bytes, size_human, sizeof size_human);

    printf("ViewFS store: %s\n",                vfs_store_path(s));
    printf("  schema:           %s\n",          vfs_store_schema(s));
    printf("  schema_version:   %d (binary expects %d)\n",
           sv, VIEWFS_SCHEMA_VERSION);
    printf("  conninfo:         %s\n",          vfs_store_conninfo(s));
    printf("  views:            %lld\n",        (long long)nviews);
    printf("  objects:          %lld (orphans: %lld)\n",
           (long long)nobjs, (long long)norph);
    printf("  mappings:         %lld\n",        (long long)nmaps);
    printf("  content storage:  %lld file(s), %s (%lld bytes)\n",
           (long long)content_files, size_human, (long long)content_bytes);

    vfs_store_close(s);
    return 0;
}
