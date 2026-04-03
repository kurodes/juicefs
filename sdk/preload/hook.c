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

/* Check if FD is in our virtual range */
static inline int is_our_fd(int fd) {
    return fd >= FD_START;
}

static int hook(long syscall_number,
                long arg0, long arg1, long arg2,
                long arg3, long arg4, long arg5,
                long *result)
{
    if (!initialized) return 1;

    switch (syscall_number) {

    /* ===== Path-based syscalls: filter by mount point prefix ===== */

    case SYS_rename: {
        char *old_path = (char *)arg0;
        char *new_path = (char *)arg1;
        if (!is_our_path(old_path) || !is_our_path(new_path))
            return 1;
        break;
    }
    case SYS_renameat:
    case SYS_renameat2: {
        /* Only support AT_FDCWD for now */
        if ((int)arg0 != AT_FDCWD || (int)arg2 != AT_FDCWD)
            return 1;
        char *old_path = (char *)arg1;
        char *new_path = (char *)arg3;
        if (!is_our_path(old_path) || !is_our_path(new_path))
            return 1;
        break;
    }
    case SYS_link: {
        char *old_path = (char *)arg0;
        char *new_path = (char *)arg1;
        if (!is_our_path(old_path) || !is_our_path(new_path))
            return 1;
        break;
    }
    case SYS_symlink: {
        /* target can be anything, but linkpath must be ours */
        char *linkpath = (char *)arg1;
        if (!is_our_path(linkpath))
            return 1;
        break;
    }
    case SYS_mkdir:
    case SYS_access:
    case SYS_stat:
    case SYS_lstat:
    case SYS_rmdir:
    case SYS_unlink:
    case SYS_statfs:
    case SYS_chmod:
    case SYS_readlink:
    case SYS_truncate:
    case SYS_open: {
        char *path = (char *)arg0;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_creat: {
        char *path = (char *)arg0;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_chown:
    case SYS_lchown: {
        char *path = (char *)arg0;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_openat: {
        /* Support AT_FDCWD + absolute path, or absolute path with any dirfd */
        char *path = (char *)arg1;
        if ((int)arg0 != AT_FDCWD && path[0] != '/')
            return 1;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_mkdirat: {
        if ((int)arg0 != AT_FDCWD)
            return 1;
        char *path = (char *)arg1;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_unlinkat: {
        if ((int)arg0 != AT_FDCWD)
            return 1;
        char *path = (char *)arg1;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_fchmodat: {
        if ((int)arg0 != AT_FDCWD)
            return 1;
        char *path = (char *)arg1;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_fchownat: {
        if ((int)arg0 != AT_FDCWD)
            return 1;
        char *path = (char *)arg1;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_newfstatat: {
        if ((int)arg0 != AT_FDCWD)
            return 1;
        char *path = (char *)arg1;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_faccessat:
#ifdef SYS_faccessat2
    case SYS_faccessat2:
#endif
    {
        if ((int)arg0 != AT_FDCWD)
            return 1;
        char *path = (char *)arg1;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_utimensat: {
        if ((int)arg0 != AT_FDCWD)
            return 1;
        char *path = (char *)arg1;
        if (path == NULL) return 1; /* fd-based utimensat */
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_readlinkat: {
        if ((int)arg0 != AT_FDCWD)
            return 1;
        char *path = (char *)arg1;
        if (!is_our_path(path))
            return 1;
        break;
    }
    case SYS_linkat: {
        if ((int)arg0 != AT_FDCWD || (int)arg2 != AT_FDCWD)
            return 1;
        char *old_path = (char *)arg1;
        char *new_path = (char *)arg3;
        if (!is_our_path(old_path) || !is_our_path(new_path))
            return 1;
        break;
    }
    case SYS_symlinkat: {
        if ((int)arg1 != AT_FDCWD)
            return 1;
        char *linkpath = (char *)arg2;
        if (!is_our_path(linkpath))
            return 1;
        break;
    }

    /* ===== FD-based syscalls: filter by virtual FD range ===== */

    case SYS_lseek:
    case SYS_read:
    case SYS_write:
    case SYS_fsync:
    case SYS_fdatasync:
    case SYS_close:
    case SYS_fstat:
    case SYS_ftruncate: {
        int fd = (int)arg0;
        if (!is_our_fd(fd))
            return 1;
        break;
    }
    case SYS_getdents64: {
        int fd = (int)arg0;
        if (!is_our_fd(fd))
            return 1;
        break;
    }

    default:
        return 1;
    }

    /* ===== Dispatch intercepted syscalls to Go functions ===== */

    switch (syscall_number) {

    /* --- Metadata operations --- */

    case SYS_mkdir:
        *result = jfs_hook_mkdir(strip_prefix((char *)arg0), (unsigned int)arg1);
        break;

    case SYS_mkdirat:
        *result = jfs_hook_mkdir(strip_prefix((char *)arg1), (unsigned int)arg2);
        break;

    case SYS_access:
        *result = jfs_hook_access(strip_prefix((char *)arg0), (int)arg1);
        break;

    case SYS_faccessat:
#ifdef SYS_faccessat2
    case SYS_faccessat2:
#endif
        *result = jfs_hook_access(strip_prefix((char *)arg1), (int)arg2);
        break;

    case SYS_stat:
        *result = jfs_hook_stat(strip_prefix((char *)arg0), (struct stat *)arg1);
        break;

    case SYS_lstat:
        *result = jfs_hook_lstat(strip_prefix((char *)arg0), (struct stat *)arg1);
        break;

    case SYS_newfstatat: {
        int flags = (int)arg3;
        if (flags & AT_SYMLINK_NOFOLLOW)
            *result = jfs_hook_lstat(strip_prefix((char *)arg1), (struct stat *)arg2);
        else
            *result = jfs_hook_stat(strip_prefix((char *)arg1), (struct stat *)arg2);
        break;
    }

    case SYS_rmdir:
        *result = jfs_hook_rmdir(strip_prefix((char *)arg0));
        break;

    case SYS_unlink:
        *result = jfs_hook_unlink(strip_prefix((char *)arg0));
        break;

    case SYS_unlinkat: {
        int flags = (int)arg2;
        if (flags & AT_REMOVEDIR)
            *result = jfs_hook_rmdir(strip_prefix((char *)arg1));
        else
            *result = jfs_hook_unlink(strip_prefix((char *)arg1));
        break;
    }

    case SYS_rename:
        *result = jfs_hook_rename(strip_prefix((char *)arg0), strip_prefix((char *)arg1));
        break;

    case SYS_renameat:
    case SYS_renameat2:
        *result = jfs_hook_rename(strip_prefix((char *)arg1), strip_prefix((char *)arg3));
        break;

    case SYS_link:
        *result = jfs_hook_link(strip_prefix((char *)arg0), strip_prefix((char *)arg1));
        break;

    case SYS_linkat:
        *result = jfs_hook_link(strip_prefix((char *)arg1), strip_prefix((char *)arg3));
        break;

    case SYS_symlink:
        *result = jfs_hook_symlink((char *)arg0, strip_prefix((char *)arg1));
        break;

    case SYS_symlinkat:
        *result = jfs_hook_symlink((char *)arg0, strip_prefix((char *)arg2));
        break;

    case SYS_readlink:
        *result = jfs_hook_readlink(strip_prefix((char *)arg0), (char *)arg1, (int)arg2);
        break;

    case SYS_readlinkat:
        *result = jfs_hook_readlink(strip_prefix((char *)arg1), (char *)arg2, (int)arg3);
        break;

    case SYS_chmod:
        *result = jfs_hook_chmod(strip_prefix((char *)arg0), (unsigned int)arg1);
        break;

    case SYS_fchmodat:
        *result = jfs_hook_chmod(strip_prefix((char *)arg1), (unsigned int)arg2);
        break;

    case SYS_chown:
    case SYS_lchown:
        *result = jfs_hook_chown(strip_prefix((char *)arg0), (unsigned int)arg1, (unsigned int)arg2);
        break;

    case SYS_fchownat:
        *result = jfs_hook_chown(strip_prefix((char *)arg1), (unsigned int)arg2, (unsigned int)arg3);
        break;

    case SYS_truncate:
        *result = jfs_hook_truncate(strip_prefix((char *)arg0), (long)arg1);
        break;

    case SYS_statfs:
        *result = jfs_hook_statfs(strip_prefix((char *)arg0), (struct statvfs *)arg1);
        break;

    case SYS_utimensat: {
        char *path = strip_prefix((char *)arg1);
        struct timespec *times = (struct timespec *)arg2;
        if (times != NULL) {
            *result = jfs_hook_utimens(path,
                times[0].tv_sec, times[0].tv_nsec,
                times[1].tv_sec, times[1].tv_nsec);
        } else {
            /* NULL means set to current time */
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            *result = jfs_hook_utimens(path,
                now.tv_sec, now.tv_nsec,
                now.tv_sec, now.tv_nsec);
        }
        break;
    }

    /* --- File operations --- */

    case SYS_open: {
        char *path = strip_prefix((char *)arg0);
        int flags = (int)arg1;
        unsigned int mode = (unsigned int)arg2;
        if (flags & (O_CREAT | O_TRUNC)) {
            *result = jfs_hook_open(path, flags, mode);
        } else {
            *result = jfs_hook_open(path, flags, 0);
        }
        break;
    }

    case SYS_openat: {
        char *path = strip_prefix((char *)arg1);
        int flags = (int)arg2;
        unsigned int mode = (unsigned int)arg3;
        if (flags & (O_CREAT | O_TRUNC)) {
            *result = jfs_hook_open(path, flags, mode);
        } else {
            *result = jfs_hook_open(path, flags, 0);
        }
        break;
    }

    case SYS_creat:
        *result = jfs_hook_create(strip_prefix((char *)arg0), (unsigned int)arg1);
        break;

    case SYS_close:
        *result = jfs_hook_close((int)arg0);
        break;

    case SYS_read:
        *result = jfs_hook_read((int)arg0, (void *)arg1, (long)arg2);
        break;

    case SYS_write:
        *result = jfs_hook_write((int)arg0, (void *)arg1, (long)arg2);
        break;

    case SYS_lseek:
        *result = jfs_hook_lseek((int)arg0, (long)arg1, (int)arg2);
        break;

    case SYS_fsync:
    case SYS_fdatasync:
        *result = jfs_hook_fsync((int)arg0);
        break;

    case SYS_fstat:
        *result = jfs_hook_fstat((int)arg0, (struct stat *)arg1);
        break;

    case SYS_ftruncate:
        *result = jfs_hook_ftruncate((int)arg0, (long)arg1);
        break;

    case SYS_getdents64:
        *result = jfs_hook_getdents64((int)arg0, (void *)arg1, (int)arg2);
        break;

    default:
        return 1;
    }

    return 0; /* handled */
}

void jfs_preload_register_hook(void) {
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
    fprintf(stderr, "[juicefs-preload] hook registered, mount_point=%s\n", mount_point);
}

static __attribute__((constructor)) void init(void) {
    jfs_preload_register_hook();
}
