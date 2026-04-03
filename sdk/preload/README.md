# JuiceFS LD_PRELOAD SDK (`libjfs_preload.so`)

通过 `LD_PRELOAD` 机制让应用程序透明访问 JuiceFS，无需 FUSE 挂载。

## 原理

利用 Intel [libsyscall_intercept](https://github.com/pmem/syscall_intercept) 在 syscall 指令级别拦截文件系统操作，将匹配路径前缀的调用转发到 JuiceFS VFS 层（Go/CGo），其余调用透传到内核。

```
应用程序
  │ syscall 指令 (open, read, write, stat, ...)
  ▼
libsyscall_intercept (LD_PRELOAD)
  │
  ▼
hook.c — 过滤 + 路径前缀剥离
  │ 路径匹配 JFS_MOUNT_POINT → 转发
  │ 不匹配 → 透传到内核
  ▼
main.go — Go 导出函数
  │ 虚拟 FD 表管理
  ▼
JuiceFS VFS (pkg/fs/, pkg/vfs/)
  │
  ▼
元数据引擎 + 对象存储
```

## 构建

### 前置依赖

- Go 1.23+
- libsyscall_intercept（需从源码编译安装）

```bash
# 安装 libsyscall_intercept
sudo apt install cmake libcapstone-dev
git clone https://github.com/pmem/syscall_intercept.git
cd syscall_intercept && mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
make -j$(nproc) && sudo make install && sudo ldconfig
```

### 编译

```bash
cd sdk/preload
make SYSCALL_INTERCEPT_DIR=/usr/local
# 产出: libjfs_preload.so
```

可通过 `BUILD_TAGS` 控制编译进哪些后端，默认排除 gateway/webdav/hdfs 等协议层，保留所有存储和元数据引擎：

```bash
# 精简构建（只保留 Redis + 本地存储）
make SYSCALL_INTERCEPT_DIR=/usr/local \
  BUILD_TAGS="nogateway,nowebdav,nohdfs,nosftp,nonfs,nodragonfly,nocifs,nogspt,nopg,nomysql,nosqlite,notikv,nobadger,noetcd,nocos,nobos,noibmcos,noobs,nooss,noqingstor,noswift,noazure,nogs,noufile,nob2"
```

## 使用

```bash
# 1. 先用 juicefs CLI 格式化卷（只需一次）
juicefs format redis://localhost:6379/1 myvol

# 2. 设置环境变量
export JFS_META_URL=redis://localhost:6379/1
export JFS_MOUNT_POINT=/jfs

# 3. 用 LD_PRELOAD 方式运行应用
LD_PRELOAD=/path/to/libjfs_preload.so your_application

# 也可以 export LD_PRELOAD，但注意所有子进程都会加载（见下文"已知限制"）
```

## 环境变量

| 变量 | 必需 | 说明 | 示例 |
|---|---|---|---|
| `JFS_META_URL` | 是 | 元数据引擎地址 | `redis://localhost:6379/1`、`postgres://localhost/mydb` |
| `JFS_MOUNT_POINT` | 是 | 拦截的路径前缀 | `/jfs` |
| `JFS_CACHE_DIR` | 否 | 本地缓存目录 | `/tmp/jfscache` |
| `JFS_CACHE_SIZE` | 否 | 缓存大小（MiB） | `2048` |
| `JFS_LOG_LEVEL` | 否 | 日志级别 | `debug`、`info`、`warn`、`error` |
| `JFS_READ_ONLY` | 否 | 只读模式 | `1` |
| `JFS_ATTR_TIMEOUT` | 否 | 属性缓存超时 | `1s`（默认） |
| `JFS_ENTRY_TIMEOUT` | 否 | 目录项缓存超时 | `1s`（默认） |
| `JFS_IO_RETRIES` | 否 | IO 重试次数 | `10` |
| `JFS_OPEN_CACHE` | 否 | Open 缓存时间 | `0s` |

## 拦截的 Syscall

### 路径类（按 `JFS_MOUNT_POINT` 前缀过滤）

| Syscall | 说明 |
|---|---|
| `open`, `openat`, `creat` | 打开/创建文件 |
| `stat`, `lstat`, `newfstatat`, `statx` | 获取文件元数据 |
| `access`, `faccessat` | 检查权限 |
| `mkdir`, `mkdirat` | 创建目录 |
| `rmdir`, `unlinkat(AT_REMOVEDIR)` | 删除目录 |
| `unlink`, `unlinkat` | 删除文件 |
| `rename`, `renameat`, `renameat2` | 重命名 |
| `link`, `linkat` | 硬链接 |
| `symlink`, `symlinkat` | 符号链接 |
| `readlink`, `readlinkat` | 读取链接 |
| `chmod`, `fchmodat` | 修改权限 |
| `chown`, `lchown`, `fchownat` | 修改属主 |
| `truncate` | 截断文件 |
| `statfs` | 文件系统信息 |
| `utimensat` | 修改时间戳 |

### FD 类（按虚拟 FD 范围过滤，FD >= 0x4197）

| Syscall | 说明 |
|---|---|
| `read`, `write` | 读写数据 |
| `lseek` | 移动文件偏移 |
| `close` | 关闭文件 |
| `fstat` | 获取已打开文件的元数据 |
| `fsync`, `fdatasync` | 同步数据到存储 |
| `ftruncate` | 截断已打开的文件 |
| `getdents64` | 读取目录条目 |

### `*at` 系列 Syscall 的 dirfd 支持

所有 `*at` 变体（`openat`、`newfstatat`、`statx`、`mkdirat` 等）支持两种模式：
- `dirfd = AT_FDCWD` + 绝对路径（如 `/jfs/foo`）
- `dirfd = 虚拟FD` + 相对路径（如 `foo`）— 通过内部 FD 路径表拼接完整路径

## 已知限制

1. **每个进程独立初始化**：`LD_PRELOAD` 是进程级别的机制。每个加载 `.so` 的进程（包括 `ls`、`cat` 等短命令的子进程）都会独立初始化 JuiceFS（连接元数据引擎、创建 session 等）。对长运行应用（训练任务、服务进程）影响可忽略（只初始化一次），但对频繁调用短命令的场景会有额外开销。建议对短命令使用 `LD_PRELOAD=... cmd` 方式而非 `export`。

2. **不拦截直接 syscall 以外的内核交互**：`mmap` 映射文件、`io_uring` 异步 IO、`sendfile` 等不在拦截范围内。

3. **不支持相对于 CWD 的 JuiceFS 路径**：如果当前目录不在 `JFS_MOUNT_POINT` 下，`open("relative/path", ...)` 不会被拦截。必须使用绝对路径。

4. **虚拟 FD 范围**：JuiceFS 使用的虚拟文件描述符从 `0x4197`（16791）开始。如果应用本身分配了大量 FD（超过 16000+），可能产生冲突。

5. **多线程安全**：FD 表通过 `sync.Mutex` 保护，但高并发场景下可能成为瓶颈。

6. **`SYS_statx` 需要内核 >= 4.11**：在老内核上编译时 `statx` 支持会被自动跳过（`#ifdef SYS_statx`）。老系统的 glibc 也不会调用 `statx`，因此不影响运行。

7. **`dup`/`dup2`/`fcntl(F_DUPFD)` 未拦截**：复制虚拟 FD 的操作不被支持。

## 设计参考

本项目参考了 [HashMeta](https://github.com/kurodes/hashmeta) 的 `libfs_syscall.so` 实现模式：
- 相同的 `libsyscall_intercept` hook 机制
- 相同的虚拟 FD 起始地址（`0x4197`）
- 相同的路径前缀过滤 + FD 范围过滤的两阶段过滤设计

区别在于后端：HashMeta 通过 eRPC 转发到分布式元数据服务，本项目通过 CGo 直接调用 JuiceFS VFS 层（无 RPC 开销）。
