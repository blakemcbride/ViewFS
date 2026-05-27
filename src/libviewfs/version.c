#include "viewfs/viewfs.h"

#define VFS_STR_(x) #x
#define VFS_STR(x)  VFS_STR_(x)

const char *viewfs_version_string(void) {
    return VFS_STR(VIEWFS_VERSION_MAJOR) "."
           VFS_STR(VIEWFS_VERSION_MINOR) "."
           VFS_STR(VIEWFS_VERSION_PATCH);
}

const char *vfs_error_str(vfs_error e) {
    switch (e) {
    case VFS_OK:                return "ok";
    case VFS_ERR_BADARGS:       return "bad arguments";
    case VFS_ERR_NOMEM:         return "out of memory";
    case VFS_ERR_IO:            return "i/o error";
    case VFS_ERR_DB:            return "database error";
    case VFS_ERR_NOTFOUND:      return "not found";
    case VFS_ERR_EXISTS:        return "already exists";
    case VFS_ERR_PATH_RELATIVE: return "path must be absolute";
    case VFS_ERR_PATH_ESCAPE:   return "path escapes root";
    case VFS_ERR_PATH_BADCHAR:  return "path contains invalid character";
    case VFS_ERR_NOTDIR:        return "not a directory";
    case VFS_ERR_ISDIR:         return "is a directory";
    case VFS_ERR_NOTEMPTY:      return "directory not empty";
    case VFS_ERR_ACCESS:        return "access denied";
    case VFS_ERR_AMBIGUOUS:     return "ambiguous prefix";
    case VFS_ERR_CONFIG:        return "config error";
    case VFS_ERR_VERSION:       return "schema version mismatch";
    case VFS_ERR_INTERNAL:      return "internal error";
    }
    return "unknown error";
}
