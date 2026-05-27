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

/* Print a libviewfs error to stderr in the form "viewfs: <msg>" and return
 * the exit code 1. If `last` is non-NULL, prepends a context line. */
int cli_perror(vfs_store *s, vfs_error e, const char *context);

/* True if `arg` is "--help", "-h", or "help". */
int cli_is_help_request(const char *arg);

/* Subcommand entry points. */
int cmd_init   (int argc, char **argv);
int cmd_status (int argc, char **argv);
int cmd_view   (int argc, char **argv);
int cmd_object (int argc, char **argv);
int cmd_attr   (int argc, char **argv);
int cmd_tag    (int argc, char **argv);
int cmd_find   (int argc, char **argv);
int cmd_mount  (int argc, char **argv);
int cmd_unmount(int argc, char **argv);
int cmd_check  (int argc, char **argv);

#endif
