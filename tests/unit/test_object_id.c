/* Unit tests for vfs_object_id_{generate,valid}. */

#include <stdio.h>
#include <string.h>

#include "viewfs/viewfs.h"

static int failures = 0;

#define FAIL(...) do { failures++; fprintf(stderr, "FAIL " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define OK(...)   do {            printf  ("ok   " __VA_ARGS__); putchar('\n');          } while (0)

int main(void) {
    /* generate two ids; they must be valid and distinct. */
    vfs_object_id a, b;
    if (vfs_object_id_generate(&a) != VFS_OK) FAIL("gen a");
    else OK("generate first id  -> %s", a.hex);
    if (vfs_object_id_generate(&b) != VFS_OK) FAIL("gen b");
    else OK("generate second id -> %s", b.hex);

    if (strlen(a.hex) != VFS_OID_HEX_LEN) FAIL("len of a != %d", VFS_OID_HEX_LEN);
    else OK("len of a is %d", VFS_OID_HEX_LEN);
    if (strlen(b.hex) != VFS_OID_HEX_LEN) FAIL("len of b != %d", VFS_OID_HEX_LEN);
    else OK("len of b is %d", VFS_OID_HEX_LEN);

    if (!strcmp(a.hex, b.hex)) FAIL("two generated ids collided");
    else OK("two generated ids differ");

    if (!vfs_object_id_valid(a.hex)) FAIL("generated id 'a' should be valid");
    else OK("a is valid");
    if (!vfs_object_id_valid(b.hex)) FAIL("generated id 'b' should be valid");
    else OK("b is valid");

    /* invalid forms */
    if (vfs_object_id_valid("")) FAIL("empty string should be invalid");
    else OK("empty rejected");
    if (vfs_object_id_valid("short")) FAIL("'short' should be invalid");
    else OK("short rejected");
    if (vfs_object_id_valid("0123456789abcdef0123456789abcdef0"))
        FAIL("33-char id should be invalid");
    else OK("33-char rejected");
    if (vfs_object_id_valid("0123456789ABCDEF0123456789abcdef"))
        FAIL("uppercase id should be invalid");
    else OK("uppercase rejected");
    if (vfs_object_id_valid("g123456789abcdef0123456789abcdef"))
        FAIL("non-hex character should be invalid");
    else OK("non-hex rejected");
    if (vfs_object_id_valid(NULL)) FAIL("NULL should be invalid");
    else OK("NULL rejected");

    if (failures) {
        fprintf(stderr, "\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nAll object_id tests passed.\n");
    return 0;
}
