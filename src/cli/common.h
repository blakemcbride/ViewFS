#ifndef VIEWFS_CLI_COMMON_H
#define VIEWFS_CLI_COMMON_H

#include "viewfs/viewfs.h"

/* Open a store from --store PATH (consumed out of argc/argv) or VIEWFS_STORE.
 * On error, prints a message to stderr and returns NULL with *exit_rc set.
 * The caller must close the returned store. */
vfs_store *cli_open_store(int *argc, char **argv, int *exit_rc);

/* Extract --flag VALUE or --flag=VALUE out of argv (mutates argc/argv).
 * Returns the value pointer (caller-owned if env returned), or NULL.
 * `boolean=1` treats the flag as a switch and writes "1" / NULL. */
const char *cli_take_flag(int *argc, char **argv, const char *flag, int boolean);

/* Print a libviewfs error to stderr in the form "vfs: <msg>" and return
 * the exit code 1. If `last` is non-NULL, prepends a context line. */
int cli_perror(vfs_store *s, vfs_error e, const char *context);

/* True if `arg` is "--help", "-h", or "help". */
int cli_is_help_request(const char *arg);

/* Resolve an object id or unique prefix, printing a clear message and
 * returning 1 on NOTFOUND/AMBIGUOUS/error, 0 on success. */
int cli_resolve_object(vfs_store *s, const char *arg, vfs_object_id *id);

/* If the current working directory is inside a mounted ViewFS view, fill
 * view_out (size vsz) with the view name and dir_out (size dsz) with the
 * canonical in-view directory path, then return 0. Returns -1 if the cwd is
 * not under any ViewFS mount. The mount is identified by its "viewfs:<view>"
 * device in /proc/mounts; the store is still taken from --store/VIEWFS_STORE. */
int cli_view_dir_from_cwd(char *view_out, size_t vsz, char *dir_out, size_t dsz);

/* Enumerate every mounted ViewFS view (a /proc/mounts entry whose device is
 * "viewfs:<view>"). Calls cb once per mount with the view name, the
 * mountpoint, and a read-only flag. Returns the number of mounts found, or
 * -1 if /proc/mounts could not be read. */
int cli_foreach_viewfs_mount(
    void (*cb)(const char *view, const char *mountpoint, int read_only, void *ud),
    void *ud);

/* Resolve the (view, directory) a `dir` subcommand should act on, from the
 * number of leading positionals it was given:
 *   n==2 : explicit -> view=a0, dir=canonicalize(a1)
 *   n==1 : view inferred from cwd; dir = a0 resolved like a shell path —
 *          absolute (leading '/') is view-relative-to-root, otherwise it is
 *          taken relative to the current in-view directory
 *   n==0 : view and dir both inferred from cwd
 * On success fills view_out/dir_out and returns 0. On failure (not inside a
 * mount when cwd is needed, or a bad path) prints a message and returns -1. */
int cli_resolve_dir(int n, const char *a0, const char *a1,
                    char *view_out, size_t vsz, char *dir_out, size_t dsz);

/* Subcommand entry points. */
int cmd_init   (int argc, char **argv);
int cmd_status (int argc, char **argv);
int cmd_view   (int argc, char **argv);
int cmd_dir    (int argc, char **argv);
int cmd_object (int argc, char **argv);
int cmd_prop   (int argc, char **argv);
int cmd_find   (int argc, char **argv);
int cmd_mount  (int argc, char **argv);
int cmd_mounts (int argc, char **argv);
int cmd_unmount(int argc, char **argv);
int cmd_check  (int argc, char **argv);

#endif
