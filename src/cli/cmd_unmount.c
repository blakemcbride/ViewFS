#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

int cmd_unmount(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: viewfs unmount MOUNTPOINT\n");
        return 2;
    }
    const char *mountpoint = argv[2];
    char *cargv[] = { (char*)"fusermount3", (char*)"-u",
                       (char*)mountpoint, NULL };
    execvp("fusermount3", cargv);
    fprintf(stderr, "viewfs: failed to exec fusermount3: %s\n", strerror(errno));
    return 127;
}
