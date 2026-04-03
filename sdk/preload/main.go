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

package main

/*
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

// Forward declarations for hook.c
void jfs_preload_register_hook(void);
*/
import "C"
import (
	"os"
	"runtime/debug"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"unsafe"

	"github.com/juicedata/juicefs/cmd"
	"github.com/juicedata/juicefs/pkg/chunk"
	"github.com/juicedata/juicefs/pkg/fs"
	"github.com/juicedata/juicefs/pkg/meta"
	"github.com/juicedata/juicefs/pkg/object"
	"github.com/juicedata/juicefs/pkg/utils"
	"github.com/juicedata/juicefs/pkg/version"
	"github.com/juicedata/juicefs/pkg/vfs"
	"github.com/sirupsen/logrus"
)

const (
	fdStart    = 0x4197 // 16791, same as HashMeta to avoid kernel FD conflicts
	maxOpenFDs = 65536
)

var (
	logger     = utils.GetLogger("juicefs")
	jfsOnce    sync.Once
	jfs        *fs.FileSystem
	metaClient meta.Meta

	// FD table: maps virtual FD -> *fs.File
	fdLock   sync.Mutex
	fdTable  = make(map[C.int]*fs.File)
	fdPaths  = make(map[C.int]string) // virtual FD -> JuiceFS path (for *at syscalls)
	nextFD   = C.int(fdStart)
	freeFDs  []C.int // recycled FDs

	// mount point prefix (set from JFS_MOUNT_POINT env var)
	mountPoint string
)

func getCtx() meta.Context {
	pid := uint32(os.Getpid())
	uid := uint32(os.Getuid())
	gid := uint32(os.Getgid())
	return meta.NewContext(pid, uid, []uint32{gid})
}

func allocFD(f *fs.File, path string) C.int {
	fdLock.Lock()
	defer fdLock.Unlock()
	var fd C.int
	if len(freeFDs) > 0 {
		fd = freeFDs[len(freeFDs)-1]
		freeFDs = freeFDs[:len(freeFDs)-1]
	} else {
		fd = nextFD
		nextFD++
	}
	fdTable[fd] = f
	fdPaths[fd] = path
	return fd
}

func getFile(fd C.int) *fs.File {
	fdLock.Lock()
	defer fdLock.Unlock()
	return fdTable[fd]
}

func freeFD(fd C.int) *fs.File {
	fdLock.Lock()
	defer fdLock.Unlock()
	f := fdTable[fd]
	if f != nil {
		delete(fdTable, fd)
		delete(fdPaths, fd)
		freeFDs = append(freeFDs, fd)
	}
	return f
}

// Convert syscall.Errno to negative errno for C (returning -errno on error, 0 on success)
func negErrno(err syscall.Errno) C.int {
	if err == 0 {
		return 0
	}
	return C.int(-err)
}

func initJFS() {
	jfsOnce.Do(func() {
		debug.SetGCPercent(50)
		object.UserAgent = "JuiceFS-Preload " + version.Version()

		metaURL := os.Getenv("JFS_META_URL")
		if metaURL == "" {
			logger.Fatalf("JFS_META_URL environment variable is required")
		}
		mountPoint = os.Getenv("JFS_MOUNT_POINT")
		if mountPoint == "" {
			logger.Fatalf("JFS_MOUNT_POINT environment variable is required")
		}
		// Ensure mount point ends without trailing slash for consistent prefix matching
		mountPoint = strings.TrimRight(mountPoint, "/")

		// Log level
		if os.Getenv("JUICEFS_DEBUG") != "" {
			utils.SetLogLevel(logrus.DebugLevel)
		} else if lvl := os.Getenv("JFS_LOG_LEVEL"); lvl != "" {
			level, err := logrus.ParseLevel(lvl)
			if err == nil {
				utils.SetLogLevel(level)
			} else {
				utils.SetLogLevel(logrus.WarnLevel)
			}
		} else {
			utils.SetLogLevel(logrus.WarnLevel)
		}

		// Metadata config
		metaConf := meta.DefaultConf()
		if v := os.Getenv("JFS_IO_RETRIES"); v != "" {
			if n, err := strconv.Atoi(v); err == nil {
				metaConf.Retries = n
			}
		}
		if os.Getenv("JFS_READ_ONLY") != "" {
			metaConf.ReadOnly = true
		}
		if v := os.Getenv("JFS_OPEN_CACHE"); v != "" {
			metaConf.OpenCache = utils.Duration(v)
		}

		m := meta.NewClient(metaURL, metaConf)
		metaClient = m
		format, err := m.Load(true)
		if err != nil {
			logger.Fatalf("load setting: %s", err)
		}

		blob, err := cmd.NewReloadableStorage(format, m, nil)
		if err != nil {
			logger.Fatalf("object storage: %s", err)
		}
		logger.Infof("Data use %s", blob)

		// Chunk config
		var freeSpaceRatio float64 = 0.1
		chunkConf := chunk.Config{
			BlockSize:      format.BlockSize * 1024,
			Compress:       format.Compression,
			CacheDir:       getEnvDefault("JFS_CACHE_DIR", "jfscache"),
			CacheMode:      0644,
			CacheSize:      parseCacheSize(),
			FreeSpace:      float32(freeSpaceRatio),
			AutoCreate:     true,
			CacheFullBlock: true,
			CacheChecksum:  "full",
			CacheEviction:  "2-random",
			MaxUpload:      20,
			MaxDownload:    20,
			MaxRetries:     10,
			Prefetch:       1,
			BufferSize:     300 << 20, // 300 MiB
			Readahead:      0,
			HashPrefix:     format.HashPrefix,
			GetTimeout:     utils.Duration("60s"),
			PutTimeout:     utils.Duration("60s"),
		}
		chunkConf.SelfCheck(format.UUID)
		store := chunk.NewCachedStore(blob, chunkConf, nil)
		m.OnMsg(meta.DeleteSlice, func(args ...interface{}) error {
			id := args[0].(uint64)
			length := args[1].(uint32)
			return store.Remove(id, int(length))
		})
		m.OnMsg(meta.CompactChunk, func(args ...interface{}) error {
			slices := args[0].([]meta.Slice)
			id := args[1].(uint64)
			return vfs.Compact(chunkConf, store, slices, id)
		})
		if err := m.NewSession(true); err != nil {
			logger.Fatalf("new session: %s", err)
		}
		m.OnReload(func(fmt *meta.Format) {
			store.UpdateLimit(fmt.UploadLimit, fmt.DownloadLimit)
		})

		// VFS config
		conf := &vfs.Config{
			Meta:         metaConf,
			Format:       *format,
			Chunk:        &chunkConf,
			AttrTimeout:  utils.Duration(getEnvDefault("JFS_ATTR_TIMEOUT", "1s")),
			EntryTimeout: utils.Duration(getEnvDefault("JFS_ENTRY_TIMEOUT", "1s")),
			FastResolve:  true,
		}

		jfs, err = fs.NewFileSystem(conf, m, store, nil)
		if err != nil {
			logger.Fatalf("initialize filesystem: %s", err)
		}
		logger.Infof("JuiceFS preload initialized: meta=%s mount=%s", metaURL, mountPoint)
	})
}

func getEnvDefault(key, defaultVal string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return defaultVal
}

func parseCacheSize() uint64 {
	v := os.Getenv("JFS_CACHE_SIZE")
	if v == "" {
		return 1024 << 20 // 1 GiB default
	}
	return utils.ParseBytesStr("cache-size", v, 'M')
}

// ============================================================================
// Exported Go functions called by hook.c
// ============================================================================

//export jfs_hook_init
func jfs_hook_init() {
	initJFS()
}

//export jfs_hook_open
func jfs_hook_open(cpath *C.char, flags C.int, mode C.uint) C.int {
	path := C.GoString(cpath)
	f, eno := jfs.Open(getCtx(), path, uint32(flags))
	if eno != 0 {
		return negErrno(eno)
	}
	return allocFD(f, path)
}

//export jfs_hook_create
func jfs_hook_create(cpath *C.char, mode C.uint) C.int {
	path := C.GoString(cpath)
	umask := uint16(syscall.Umask(0))
	syscall.Umask(int(umask))
	f, eno := jfs.Create(getCtx(), path, uint16(mode), umask)
	if eno != 0 {
		return negErrno(eno)
	}
	return allocFD(f, path)
}

//export jfs_hook_fd_path
func jfs_hook_fd_path(fd C.int, cbuf *C.char, bufsiz C.int) C.int {
	fdLock.Lock()
	p, ok := fdPaths[fd]
	fdLock.Unlock()
	if !ok {
		return -1
	}
	if len(p) >= int(bufsiz) {
		return -1
	}
	buf := unsafe.Slice((*byte)(unsafe.Pointer(cbuf)), int(bufsiz))
	copy(buf, p)
	buf[len(p)] = 0
	return C.int(len(p))
}

//export jfs_hook_close
func jfs_hook_close(fd C.int) C.int {
	f := freeFD(fd)
	if f == nil {
		return -C.int(syscall.EBADF)
	}
	eno := f.Close(getCtx())
	return negErrno(eno)
}

//export jfs_hook_read
func jfs_hook_read(fd C.int, cbuf unsafe.Pointer, count C.long) C.long {
	f := getFile(fd)
	if f == nil {
		return C.long(-C.int(syscall.EBADF))
	}
	buf := unsafe.Slice((*byte)(cbuf), int(count))
	n, err := f.Read(getCtx(), buf)
	if err != nil {
		if n > 0 {
			return C.long(n)
		}
		// EOF returns 0 for read syscall
		if err.Error() == "EOF" {
			return 0
		}
		return C.long(-C.int(syscall.EIO))
	}
	return C.long(n)
}

//export jfs_hook_write
func jfs_hook_write(fd C.int, cbuf unsafe.Pointer, count C.long) C.long {
	f := getFile(fd)
	if f == nil {
		return C.long(-C.int(syscall.EBADF))
	}
	buf := unsafe.Slice((*byte)(cbuf), int(count))
	n, eno := f.Write(getCtx(), buf)
	if eno != 0 {
		if n > 0 {
			return C.long(n)
		}
		return C.long(negErrno(eno))
	}
	return C.long(n)
}

//export jfs_hook_lseek
func jfs_hook_lseek(fd C.int, offset C.long, whence C.int) C.long {
	f := getFile(fd)
	if f == nil {
		return C.long(-C.int(syscall.EBADF))
	}
	off, err := f.Seek(getCtx(), int64(offset), int(whence))
	if err != nil {
		return C.long(-C.int(syscall.EINVAL))
	}
	return C.long(off)
}

//export jfs_hook_fsync
func jfs_hook_fsync(fd C.int) C.int {
	f := getFile(fd)
	if f == nil {
		return -C.int(syscall.EBADF)
	}
	return negErrno(f.Fsync(getCtx()))
}

//export jfs_hook_fstat
func jfs_hook_fstat(fd C.int, stbuf *C.struct_stat) C.int {
	f := getFile(fd)
	if f == nil {
		return -C.int(syscall.EBADF)
	}
	info, err := f.Stat()
	if err != nil {
		return -C.int(syscall.EIO)
	}
	fillStat(stbuf, info.(*fs.FileStat))
	return 0
}

//export jfs_hook_ftruncate
func jfs_hook_ftruncate(fd C.int, length C.long) C.int {
	f := getFile(fd)
	if f == nil {
		return -C.int(syscall.EBADF)
	}
	return negErrno(f.Truncate(getCtx(), uint64(length)))
}

//export jfs_hook_stat
func jfs_hook_stat(cpath *C.char, stbuf *C.struct_stat) C.int {
	path := C.GoString(cpath)
	fi, eno := jfs.Stat(getCtx(), path)
	if eno != 0 {
		return negErrno(eno)
	}
	fillStat(stbuf, fi)
	return 0
}

//export jfs_hook_lstat
func jfs_hook_lstat(cpath *C.char, stbuf *C.struct_stat) C.int {
	path := C.GoString(cpath)
	fi, eno := jfs.Lstat(getCtx(), path)
	if eno != 0 {
		return negErrno(eno)
	}
	fillStat(stbuf, fi)
	return 0
}

//export jfs_hook_access
func jfs_hook_access(cpath *C.char, mode C.int) C.int {
	path := C.GoString(cpath)
	return negErrno(jfs.Access(getCtx(), path, int(mode)))
}

//export jfs_hook_mkdir
func jfs_hook_mkdir(cpath *C.char, mode C.uint) C.int {
	path := C.GoString(cpath)
	umask := uint16(syscall.Umask(0))
	syscall.Umask(int(umask))
	return negErrno(jfs.Mkdir(getCtx(), path, uint16(mode), umask))
}

//export jfs_hook_rmdir
func jfs_hook_rmdir(cpath *C.char) C.int {
	path := C.GoString(cpath)
	return negErrno(jfs.Rmdir(getCtx(), path))
}

//export jfs_hook_unlink
func jfs_hook_unlink(cpath *C.char) C.int {
	path := C.GoString(cpath)
	return negErrno(jfs.Unlink(getCtx(), path))
}

//export jfs_hook_rename
func jfs_hook_rename(coldpath *C.char, cnewpath *C.char) C.int {
	oldpath := C.GoString(coldpath)
	newpath := C.GoString(cnewpath)
	return negErrno(jfs.Rename(getCtx(), oldpath, newpath, 0))
}

//export jfs_hook_link
func jfs_hook_link(coldpath *C.char, cnewpath *C.char) C.int {
	oldpath := C.GoString(coldpath)
	newpath := C.GoString(cnewpath)
	return negErrno(jfs.Link(getCtx(), oldpath, newpath))
}

//export jfs_hook_symlink
func jfs_hook_symlink(ctarget *C.char, clinkpath *C.char) C.int {
	target := C.GoString(ctarget)
	linkpath := C.GoString(clinkpath)
	return negErrno(jfs.Symlink(getCtx(), target, linkpath))
}

//export jfs_hook_readlink
func jfs_hook_readlink(cpath *C.char, cbuf *C.char, bufsiz C.int) C.int {
	path := C.GoString(cpath)
	target, eno := jfs.Readlink(getCtx(), path)
	if eno != 0 {
		return negErrno(eno)
	}
	n := len(target)
	if n > int(bufsiz) {
		n = int(bufsiz)
	}
	buf := unsafe.Slice((*byte)(unsafe.Pointer(cbuf)), int(bufsiz))
	copy(buf, target[:n])
	return C.int(n)
}

//export jfs_hook_chmod
func jfs_hook_chmod(cpath *C.char, mode C.uint) C.int {
	path := C.GoString(cpath)
	ctx := getCtx()
	f, eno := jfs.Open(ctx, path, 0)
	if eno != 0 {
		return negErrno(eno)
	}
	defer f.Close(ctx)
	return negErrno(f.Chmod(ctx, uint16(mode)))
}

//export jfs_hook_chown
func jfs_hook_chown(cpath *C.char, uid C.uint, gid C.uint) C.int {
	path := C.GoString(cpath)
	ctx := getCtx()
	f, eno := jfs.Open(ctx, path, 0)
	if eno != 0 {
		return negErrno(eno)
	}
	defer f.Close(ctx)
	return negErrno(f.Chown(ctx, uint32(uid), uint32(gid)))
}

//export jfs_hook_truncate
func jfs_hook_truncate(cpath *C.char, length C.long) C.int {
	path := C.GoString(cpath)
	return negErrno(jfs.Truncate(getCtx(), path, uint64(length)))
}

//export jfs_hook_utimens
func jfs_hook_utimens(cpath *C.char, atime_sec C.long, atime_nsec C.long, mtime_sec C.long, mtime_nsec C.long) C.int {
	path := C.GoString(cpath)
	ctx := getCtx()
	f, eno := jfs.Open(ctx, path, 0)
	if eno != 0 {
		return negErrno(eno)
	}
	defer f.Close(ctx)
	return negErrno(f.Utime2(ctx, int64(atime_sec), int64(atime_nsec), int64(mtime_sec), int64(mtime_nsec)))
}

//export jfs_hook_statfs
func jfs_hook_statfs(cpath *C.char, stbuf *C.struct_statvfs) C.int {
	total, avail := jfs.StatFS(getCtx())
	blockSize := C.ulong(4096)
	stbuf.f_bsize = blockSize
	stbuf.f_frsize = blockSize
	stbuf.f_blocks = C.ulong(total / uint64(blockSize))
	stbuf.f_bfree = C.ulong(avail / uint64(blockSize))
	stbuf.f_bavail = stbuf.f_bfree
	stbuf.f_namemax = 255
	return 0
}

//export jfs_hook_getdents64
func jfs_hook_getdents64(fd C.int, cbuf unsafe.Pointer, count C.int) C.int {
	f := getFile(fd)
	if f == nil {
		return -C.int(syscall.EBADF)
	}
	entries, eno := f.Readdir(getCtx(), 0)
	if eno != 0 {
		return negErrno(eno)
	}
	// Pack directory entries into linux_dirent64 format
	buf := unsafe.Slice((*byte)(cbuf), int(count))
	offset := 0
	for _, entry := range entries {
		name := entry.Name()
		// linux_dirent64: d_ino(8) + d_off(8) + d_reclen(2) + d_type(1) + d_name(variable + null)
		reclen := (8 + 8 + 2 + 1 + len(name) + 1 + 7) & ^7 // align to 8 bytes
		if offset+reclen > int(count) {
			break
		}
		// d_ino
		*(*uint64)(unsafe.Pointer(&buf[offset])) = 1 // placeholder inode
		// d_off
		*(*uint64)(unsafe.Pointer(&buf[offset+8])) = uint64(offset + reclen)
		// d_reclen
		*(*uint16)(unsafe.Pointer(&buf[offset+16])) = uint16(reclen)
		// d_type
		mode := entry.Mode()
		var dtype byte
		switch {
		case mode.IsDir():
			dtype = 4 // DT_DIR
		case mode.IsRegular():
			dtype = 8 // DT_REG
		case mode&os.ModeSymlink != 0:
			dtype = 10 // DT_LNK
		default:
			dtype = 0 // DT_UNKNOWN
		}
		buf[offset+18] = dtype
		// d_name
		copy(buf[offset+19:], name)
		buf[offset+19+len(name)] = 0 // null terminator
		offset += reclen
	}
	return C.int(offset)
}

func fillStat(stbuf *C.struct_stat, fi *fs.FileStat) {
	attr := fi.Sys().(*meta.Attr)
	C.memset(unsafe.Pointer(stbuf), 0, C.sizeof_struct_stat)
	stbuf.st_ino = C.ulong(fi.Inode())
	stbuf.st_mode = C.uint(attr.SMode())
	stbuf.st_nlink = C.ulong(attr.Nlink)
	stbuf.st_uid = C.uint(attr.Uid)
	stbuf.st_gid = C.uint(attr.Gid)
	stbuf.st_size = C.long(attr.Length)
	stbuf.st_blksize = 4096
	stbuf.st_blocks = C.long((attr.Length + 511) / 512)
	stbuf.st_atim.tv_sec = C.long(attr.Atime)
	stbuf.st_atim.tv_nsec = C.long(attr.Atimensec)
	stbuf.st_mtim.tv_sec = C.long(attr.Mtime)
	stbuf.st_mtim.tv_nsec = C.long(attr.Mtimensec)
	stbuf.st_ctim.tv_sec = C.long(attr.Ctime)
	stbuf.st_ctim.tv_nsec = C.long(attr.Ctimensec)
}

func init() {
	// Register the syscall hook when this shared library is loaded.
	// The Go init() runs before C constructors in CGo c-shared mode,
	// but we also call from C constructor as a safety net.
}

func main() {
	// Required for -buildmode=c-shared but never called
}
