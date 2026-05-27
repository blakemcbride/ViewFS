#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
