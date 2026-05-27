/* Unit tests for vfs_path_canonicalize. */

#include <stdio.h>
#include <string.h>

#include "viewfs/viewfs.h"

static int failures = 0;

static void check(const char *input, vfs_error expected_rc,
                  const char *expected_path,
                  const char *expected_parent,
                  const char *expected_name,
                  int expected_is_root) {
    vfs_canon_path out;
    vfs_error rc = vfs_path_canonicalize(input, &out);
    int ok = (rc == expected_rc);
    if (ok && rc == VFS_OK) {
        ok = ok && !strcmp(out.path,   expected_path)   &&
                   !strcmp(out.parent, expected_parent) &&
                   !strcmp(out.name,   expected_name)   &&
                   (out.is_root == expected_is_root);
    }
    if (!ok) {
        failures++;
        fprintf(stderr,
            "FAIL  input=%-40s  got rc=%d path=\"%s\" parent=\"%s\" name=\"%s\"\n",
            input ? input : "(null)", rc,
            (rc == VFS_OK) ? out.path : "",
            (rc == VFS_OK) ? out.parent : "",
            (rc == VFS_OK) ? out.name : "");
    } else {
        printf("ok    %s\n", input ? input : "(null)");
    }
}

int main(void) {
    /* root */
    check("/",            VFS_OK, "/", "", "", 1);
    check("//",           VFS_OK, "/", "", "", 1);
    check("/./",          VFS_OK, "/", "", "", 1);

    /* simple children */
    check("/a",           VFS_OK, "/a", "", "a", 0);
    check("/a/",          VFS_OK, "/a", "", "a", 0);
    check("/a/b",         VFS_OK, "/a/b", "/a", "b", 0);
    check("/a/b/c",       VFS_OK, "/a/b/c", "/a/b", "c", 0);

    /* duplicate slashes */
    check("/a//b",        VFS_OK, "/a/b", "/a", "b", 0);
    check("//a///b/",     VFS_OK, "/a/b", "/a", "b", 0);

    /* . segments */
    check("/a/./b",       VFS_OK, "/a/b", "/a", "b", 0);
    check("/./a",         VFS_OK, "/a", "", "a", 0);

    /* .. segments */
    check("/a/b/..",      VFS_OK, "/a", "", "a", 0);
    check("/a/b/../c",    VFS_OK, "/a/c", "/a", "c", 0);
    check("/a/../b",      VFS_OK, "/b", "", "b", 0);

    /* escapes past root */
    check("/..",          VFS_ERR_PATH_ESCAPE, "", "", "", 0);
    check("/a/../..",     VFS_ERR_PATH_ESCAPE, "", "", "", 0);
    check("/a/../../b",   VFS_ERR_PATH_ESCAPE, "", "", "", 0);

    /* relative paths */
    check("",             VFS_ERR_PATH_RELATIVE, "", "", "", 0);
    check("a",            VFS_ERR_PATH_RELATIVE, "", "", "", 0);
    check("a/b",          VFS_ERR_PATH_RELATIVE, "", "", "", 0);

    /* embedded control chars */
    char bad[] = { '/', 'a', '\x01', 'b', '\0' };
    check(bad,            VFS_ERR_PATH_BADCHAR, "", "", "", 0);

    /* extremely deep path that should still fit */
    char deep[200];
    deep[0] = '/';
    for (int i = 1; i < 198; i += 2) { deep[i] = 'a'; deep[i+1] = '/'; }
    deep[197] = 'x'; deep[198] = '\0';
    {
        vfs_canon_path out;
        vfs_error rc = vfs_path_canonicalize(deep, &out);
        if (rc != VFS_OK) {
            failures++;
            fprintf(stderr, "FAIL  deep path rc=%d\n", rc);
        } else {
            printf("ok    deep path\n");
        }
    }

    if (failures) {
        fprintf(stderr, "\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nAll canonicalize tests passed.\n");
    return 0;
}
