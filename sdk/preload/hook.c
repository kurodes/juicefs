/*
 * JuiceFS, Copyright 2024 Juicedata, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include "libsyscall_intercept_hook_point.h"

/* Virtual FD range: same as HashMeta */
#define FD_START 0x4197

/* Mount point prefix (read from JFS_MOUNT_POINT env var) */
static char mount_point[4096];
static int mount_point_len = 0;
static int initialized = 0;

/* Go-exported functions (from _cgo_export.h at build time) */
extern void jfs_hook_init(void);
extern int jfs_hook_open(char *path, int flags, unsigned int mode);
extern int jfs_hook_create(char *path, unsigned int mode);
extern int jfs_hook_close(int fd);
extern long jfs_hook_read(int fd, void *buf, long count);
extern long jfs_hook_write(int fd, void *buf, long count);
extern long jfs_hook_lseek(int fd, long offset, int whence);
extern int jfs_hook_fsync(int fd);
extern int jfs_hook_fstat(int fd, struct stat *stbuf);
extern int jfs_hook_ftruncate(int fd, long length);
extern int jfs_hook_stat(char *path, struct stat *stbuf);
extern int jfs_hook_lstat(char *path, struct stat *stbuf);
extern int jfs_hook_access(char *path, int mode);
extern int jfs_hook_mkdir(char *path, unsigned int mode);
extern int jfs_hook_rmdir(char *path);
extern int jfs_hook_unlink(char *path);
extern int jfs_hook_rename(char *oldpath, char *newpath);
extern int jfs_hook_link(char *oldpath, char *newpath);
extern int jfs_hook_symlink(char *target, char *linkpath);
extern int jfs_hook_readlink(char *path, char *buf, int bufsiz);
extern int jfs_hook_chmod(char *path, unsigned int mode);
extern int jfs_hook_chown(char *path, unsigned int uid, unsigned int gid);
extern int jfs_hook_truncate(char *path, long length);
extern int jfs_hook_utimens(char *path, long atime_sec, long atime_nsec, long mtime_sec, long mtime_nsec);
extern int jfs_hook_statfs(char *path, struct statvfs *stbuf);
extern int jfs_hook_getdents64(int fd, void *buf, int count);
extern int jfs_hook_statx(char *path, int flags, unsigned int mask, void *statxbuf);
extern int jfs_hook_fd_path(int fd, char *buf, int bufsiz);

/* Check if FD is in our virtual range */
static inline int is_our_fd(int fd) {
    return fd >= FD_START;
}

/* Check if a path is under our mount point */
static inline int is_our_path(const char *path) {
    if (mount_point_len == 0) return 0;
    if (strncmp(path, mount_point, mount_point_len) != 0) return 0;
    /* Must be exact match or followed by '/' */
    char c = path[mount_point_len];
    return (c == '\0' || c == '/');
}

/* Strip mount point prefix, returning the relative path within JuiceFS.
 * "/jfs/foo/bar" -> "/foo/bar", "/jfs" -> "/" */
static inline char *strip_prefix(const char *path) {
    const char *rel = path + mount_point_len;
    if (*rel == '\0') return "/";
    return (char *)rel; /* already starts with '/' */
}

/*
 * Resolve a *at(dirfd, relative_name) into a full JuiceFS path.
 * If dirfd is AT_FDCWD and path is absolute under mount_point, strip prefix.
 * If dirfd is a virtual FD, look up its path and join with the relative name.
 * Returns the JuiceFS-internal path in 'out', or NULL if not our path.
 */
static char *resolve_at(int dirfd, const char *pathname, char *out, int outsz) {
    if (pathname && pathname[0] == '/') {
        /* Absolute path — ignore dirfd */
        if (!is_our_path(pathname)) return NULL;
        return strip_prefix(pathname);
    }
    /* Relative path — need dirfd */
    if (dirfd == AT_FDCWD) return NULL; /* relative to cwd, not ours */
    if (!is_our_fd(dirfd)) return NULL;

    char dirpath[4096];
    int dlen = jfs_hook_fd_path(dirfd, dirpath, sizeof(dirpath));
    if (dlen < 0) return NULL;

    if (pathname == NULL || pathname[0] == '\0') {
        /* e.g. fstatat(fd, "", ..., AT_EMPTY_PATH) */
        memcpy(out, dirpath, dlen);
        out[dlen] = '\0';
        return out;
    }

    /* Join dirpath + "/" + pathname */
    int plen = strlen(pathname);
    if (dlen + 1 + plen >= outsz) return NULL;
    memcpy(out, dirpath, dlen);
    if (dlen > 1 || dirpath[0] != '/') {
        out[dlen] = '/';
        memcpy(out + dlen + 1, pathname, plen);
        out[dlen + 1 + plen] = '\0';
    } else {
        /* dirpath is "/" — avoid double slash */
        memcpy(out + 1, pathname, plen);
        out[1 + plen] = '\0';
    }
    return out;
}

/*
 * Helper: resolve a single *at(dirfd, path) argument.
 * Writes the resolved JuiceFS-internal path into 'out'.
 * Returns a pointer to the resolved path, or NULL if not ours.
 */
static char *resolve_one(int dirfd, const char *pathname, char *out, int outsz) {
    if (pathname && pathname[0] == '/') {
        if (!is_our_path(pathname)) return NULL;
        return strip_prefix(pathname);
    }
    return resolve_at(dirfd, pathname, out, outsz);
}

static int hook(long syscall_number,
                long arg0, long arg1, long arg2,
                long arg3, long arg4, long arg5,
                long *result)
{
    if (!initialized) return 1;

    char resolved1[4096], resolved2[4096];
    char *p1, *p2;

    switch (syscall_number) {

    /* ===== Legacy path-based syscalls (no dirfd) ===== */

    case SYS_mkdir:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_mkdir(strip_prefix((char *)arg0), (unsigned int)arg1);
        return 0;

    case SYS_access:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_access(strip_prefix((char *)arg0), (int)arg1);
        return 0;

    case SYS_stat:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_stat(strip_prefix((char *)arg0), (struct stat *)arg1);
        return 0;

    case SYS_lstat:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_lstat(strip_prefix((char *)arg0), (struct stat *)arg1);
        return 0;

    case SYS_rmdir:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_rmdir(strip_prefix((char *)arg0));
        return 0;

    case SYS_unlink:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_unlink(strip_prefix((char *)arg0));
        return 0;

    case SYS_statfs:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_statfs(strip_prefix((char *)arg0), (struct statvfs *)arg1);
        return 0;

    case SYS_chmod:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_chmod(strip_prefix((char *)arg0), (unsigned int)arg1);
        return 0;

    case SYS_readlink:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_readlink(strip_prefix((char *)arg0), (char *)arg1, (int)arg2);
        return 0;

    case SYS_truncate:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_truncate(strip_prefix((char *)arg0), (long)arg1);
        return 0;

    case SYS_chown:
    case SYS_lchown:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_chown(strip_prefix((char *)arg0), (unsigned int)arg1, (unsigned int)arg2);
        return 0;

    case SYS_open: {
        if (!is_our_path((char *)arg0)) return 1;
        char *path = strip_prefix((char *)arg0);
        int flags = (int)arg1;
        *result = jfs_hook_open(path, flags, (unsigned int)arg2);
        return 0;
    }

    case SYS_creat:
        if (!is_our_path((char *)arg0)) return 1;
        *result = jfs_hook_create(strip_prefix((char *)arg0), (unsigned int)arg1);
        return 0;

    case SYS_rename:
        if (!is_our_path((char *)arg0) || !is_our_path((char *)arg1)) return 1;
        *result = jfs_hook_rename(strip_prefix((char *)arg0), strip_prefix((char *)arg1));
        return 0;

    case SYS_link:
        if (!is_our_path((char *)arg0) || !is_our_path((char *)arg1)) return 1;
        *result = jfs_hook_link(strip_prefix((char *)arg0), strip_prefix((char *)arg1));
        return 0;

    case SYS_symlink:
        if (!is_our_path((char *)arg1)) return 1;
        *result = jfs_hook_symlink((char *)arg0, strip_prefix((char *)arg1));
        return 0;

    /* ===== *at syscalls: support both AT_FDCWD and virtual dirfd ===== */

    case SYS_openat: {
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        int flags = (int)arg2;
        *result = jfs_hook_open(p1, flags, (unsigned int)arg3);
        return 0;
    }

    case SYS_mkdirat:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        *result = jfs_hook_mkdir(p1, (unsigned int)arg2);
        return 0;

    case SYS_unlinkat:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        if ((int)arg2 & AT_REMOVEDIR)
            *result = jfs_hook_rmdir(p1);
        else
            *result = jfs_hook_unlink(p1);
        return 0;

    case SYS_newfstatat:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        if ((int)arg3 & AT_SYMLINK_NOFOLLOW)
            *result = jfs_hook_lstat(p1, (struct stat *)arg2);
        else
            *result = jfs_hook_stat(p1, (struct stat *)arg2);
        return 0;

#ifdef SYS_statx
    case SYS_statx:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        *result = jfs_hook_statx(p1, (int)arg2, (unsigned int)arg3, (void *)arg4);
        return 0;
#endif

    case SYS_fchmodat:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        *result = jfs_hook_chmod(p1, (unsigned int)arg2);
        return 0;

    case SYS_fchownat:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        *result = jfs_hook_chown(p1, (unsigned int)arg2, (unsigned int)arg3);
        return 0;

    case SYS_faccessat:
#ifdef SYS_faccessat2
    case SYS_faccessat2:
#endif
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        *result = jfs_hook_access(p1, (int)arg2);
        return 0;

    case SYS_readlinkat:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        *result = jfs_hook_readlink(p1, (char *)arg2, (int)arg3);
        return 0;

    case SYS_utimensat: {
        char *pathname = (char *)arg1;
        if (pathname == NULL) return 1;
        p1 = resolve_one((int)arg0, pathname, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        struct timespec *times = (struct timespec *)arg2;
        if (times != NULL) {
            *result = jfs_hook_utimens(p1,
                times[0].tv_sec, times[0].tv_nsec,
                times[1].tv_sec, times[1].tv_nsec);
        } else {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            *result = jfs_hook_utimens(p1,
                now.tv_sec, now.tv_nsec,
                now.tv_sec, now.tv_nsec);
        }
        return 0;
    }

    case SYS_renameat:
    case SYS_renameat2:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        p2 = resolve_one((int)arg2, (char *)arg3, resolved2, sizeof(resolved2));
        if (!p1 || !p2) return 1;
        *result = jfs_hook_rename(p1, p2);
        return 0;

    case SYS_linkat:
        p1 = resolve_one((int)arg0, (char *)arg1, resolved1, sizeof(resolved1));
        p2 = resolve_one((int)arg2, (char *)arg3, resolved2, sizeof(resolved2));
        if (!p1 || !p2) return 1;
        *result = jfs_hook_link(p1, p2);
        return 0;

    case SYS_symlinkat:
        p1 = resolve_one((int)arg1, (char *)arg2, resolved1, sizeof(resolved1));
        if (!p1) return 1;
        *result = jfs_hook_symlink((char *)arg0, p1);
        return 0;

    /* ===== FD-based syscalls: filter by virtual FD range ===== */

    case SYS_close:
        if (!is_our_fd((int)arg0)) return 1;
        *result = jfs_hook_close((int)arg0);
        return 0;

    case SYS_read:
        if (!is_our_fd((int)arg0)) return 1;
        *result = jfs_hook_read((int)arg0, (void *)arg1, (long)arg2);
        return 0;

    case SYS_write:
        if (!is_our_fd((int)arg0)) return 1;
        *result = jfs_hook_write((int)arg0, (void *)arg1, (long)arg2);
        return 0;

    case SYS_lseek:
        if (!is_our_fd((int)arg0)) return 1;
        *result = jfs_hook_lseek((int)arg0, (long)arg1, (int)arg2);
        return 0;

    case SYS_fsync:
    case SYS_fdatasync:
        if (!is_our_fd((int)arg0)) return 1;
        *result = jfs_hook_fsync((int)arg0);
        return 0;

    case SYS_fstat:
        if (!is_our_fd((int)arg0)) return 1;
        *result = jfs_hook_fstat((int)arg0, (struct stat *)arg1);
        return 0;

    case SYS_ftruncate:
        if (!is_our_fd((int)arg0)) return 1;
        *result = jfs_hook_ftruncate((int)arg0, (long)arg1);
        return 0;

    case SYS_getdents64:
        if (!is_our_fd((int)arg0)) return 1;
        *result = jfs_hook_getdents64((int)arg0, (void *)arg1, (int)arg2);
        return 0;

    default:
        return 1;
    }
}

static int init_done = 0;

void jfs_preload_register_hook(void) {
    if (init_done) return;
    init_done = 1;

    /* Read mount point from environment */
    const char *mp = getenv("JFS_MOUNT_POINT");
    if (mp == NULL || mp[0] == '\0') {
        fprintf(stderr, "[juicefs-preload] JFS_MOUNT_POINT not set, hook disabled\n");
        return;
    }

    /* Copy and strip trailing slashes */
    strncpy(mount_point, mp, sizeof(mount_point) - 1);
    mount_point[sizeof(mount_point) - 1] = '\0';
    mount_point_len = strlen(mount_point);
    while (mount_point_len > 1 && mount_point[mount_point_len - 1] == '/') {
        mount_point[--mount_point_len] = '\0';
    }

    /* Initialize JuiceFS VFS */
    jfs_hook_init();
    initialized = 1;

    /* Register the syscall hook */
    intercept_hook_point = hook;
    const char *log_level = getenv("JFS_LOG_LEVEL");
    if (log_level != NULL && strcmp(log_level, "debug") == 0) {
        fprintf(stderr, "[juicefs-preload] hook registered, mount_point=%s\n", mount_point);
    }
}

static __attribute__((constructor)) void init(void) {
    jfs_preload_register_hook();
}
