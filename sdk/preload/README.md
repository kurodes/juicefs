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

通过两个环境变量配置：

- `JFS_MOUNT_POINT` — 拦截的路径前缀（必需）
- `JFS_CONFIG` — JSON 格式的 JuiceFS 配置（必需）

```bash
# 1. 先用 juicefs CLI 格式化卷（只需一次）
juicefs format 'postgres://localhost:5432/mypg?sslmode=disable' myvol

# 2. 配置并运行
export JFS_MOUNT_POINT=/jfs
export JFS_CONFIG='{
  "meta": "postgres://localhost:5432/mypg?sslmode=disable",
  "cacheDir": "memory",
  "cacheSize": "0",
  "noBGJob": true,
  "backupMeta": "0",
  "bufferSize": "800M",
  "attrTimeout": "1s",
  "entryTimeout": "1s",
  "prefetch": 3,
  "fastResolve": true
}'
LD_PRELOAD=./libjfs_preload.so python3 your_app.py
```

## JFS_CONFIG 字段说明

JSON 字段名与 JuiceFS Java SDK 的配置格式完全兼容。

| 字段 | mount 对应参数 | 类型 | 说明 | 默认值 |
|---|---|---|---|---|
| `meta` | 第一个参数 | string | 元数据引擎地址（**必需**） | — |
| **缓存** | | | | |
| `cacheDir` | `--cache-dir` | string | 缓存目录（`memory` 表示纯内存） | `jfscache` |
| `cacheSize` | `--cache-size` | string | 缓存大小（MiB） | `"1024"` |
| `bufferSize` | `--buffer-size` | string | 读写缓冲区大小 | `"300M"` |
| `writeback` | `--writeback` | bool | 写回模式 | `false` |
| `prefetch` | `--prefetch` | int | 预读并发数 | `1` |
| `readahead` | `--max-readahead` | string | 最大预读大小 | `""` |
| `cacheFullBlock` | `--cache-full-block` | bool | 缓存完整块 | `true` |
| `cacheChecksum` | `--verify-cache-checksum` | string | 缓存校验 | `"full"` |
| `cacheEviction` | `--cache-eviction` | string | 缓存淘汰策略 | `"2-random"` |
| `freeSpace` | `--free-space-ratio` | string | 可用空间比例 | `"0.1"` |
| `autoCreate` | `--auto-create-cache-dir` | bool | 自动创建缓存目录 | `true` |
| **元数据缓存** | | | | |
| `attrTimeout` | `--attr-cache` | string | 属性缓存超时 | `"1s"` |
| `entryTimeout` | `--entry-cache` | string | 文件项缓存超时 | `"1s"` |
| `dirEntryTimeout` | `--dir-entry-cache` | string | 目录项缓存超时 | `"1s"` |
| `openCache` | `--open-cache` | string | Open 缓存时间 | `""` |
| **后台任务** | | | | |
| `noBGJob` | `--no-bgjob` | bool | 禁用后台任务 | `false` |
| `backupMeta` | `--backup-meta` | string | 自动备份间隔（`"0"` 禁用） | `"0"` |
| `noSession` | — | bool | 禁用 session 注册 | `false` |
| **性能调优** | | | | |
| `skipDirMtime` | `--skip-dir-mtime` | string | 跳过目录 mtime 更新窗口 | `""` |
| `skipDirNlink` | `--skip-dir-nlink` | int | 跳过目录 nlink 计算 | `0` |
| `ioRetries` | `--io-retries` | int | IO 重试次数 | `10` |
| `maxUploads` | `--max-uploads` | int | 最大上传并发 | `20` |
| `maxDownloads` | `--max-downloads` | int | 最大下载并发 | `20` |
| `uploadLimit` | `--upload-limit` | string | 上传带宽限制（Mbps） | `""` |
| `downloadLimit` | `--download-limit` | string | 下载带宽限制（Mbps） | `""` |
| `getTimeout` | `--get-timeout` | string | 对象存储 GET 超时 | `"60s"` |
| `putTimeout` | `--put-timeout` | string | 对象存储 PUT 超时 | `"60s"` |
| **其他** | | | | |
| `readOnly` | `--read-only` | bool | 只读模式 | `false` |
| `debug` | — | bool | 开启 debug 日志 | `false` |
| `logLevel` | — | string | 日志级别 | `"warn"` |
| `fastResolve` | `--fast-resolve` | bool | 快速路径解析 | `true` |
| `heartbeat` | `--heartbeat` | string | 客户端心跳间隔 | `""` |
| `bucket` | `--bucket` | string | 覆盖存储桶地址 | `""` |
| `storageClass` | `--storage-class` | string | 存储类别 | `""` |
| `subdir` | `--subdir` | string | 子目录 | `""` |
| `accessLog` | `--access-log` | string | 访问日志路径 | `""` |

### 与 mount 命令对应示例

```bash
# mount 命令:
juicefs mount 'postgres://...' /jfs --background \
  --backup-meta=0 --no-bgjob=true \
  --cache-dir=memory --cache-size=0 --buffer-size=800M

# 等价的 LD_PRELOAD 方式:
export JFS_MOUNT_POINT=/jfs
export JFS_CONFIG='{
  "meta": "postgres://localhost:5432/mypg?sslmode=disable",
  "backupMeta": "0",
  "noBGJob": true,
  "cacheDir": "memory",
  "cacheSize": "0",
  "bufferSize": "800M"
}'
LD_PRELOAD=./libjfs_preload.so python3 your_app.py
```

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

7. **`dup`/`dup2`/`fcntl(F_DUPFD)` 未拦截**：复制虚拟 FD 的操作不被支持。因此 shell 重定向（`echo "x" > /jfs/file`）不可用，请使用 Python 等应用程序直接进行文件 IO。

## 测试

提供了一个 Python 测试脚本，覆盖 AI 场景中常见的文件系统操作：

```bash
export JFS_META_URL="postgres://localhost:5432/mypg?sslmode=disable"
export JFS_MOUNT_POINT=/jfs
LD_PRELOAD=./libjfs_preload.so python3 test_preload.py
```

测试用例包括：

| # | 测试项 | 说明 |
|---|---|---|
| 1 | 基础目录操作 | makedirs, listdir, exists, isdir |
| 2 | 文本文件读写 | write, read, append, 内容校验 |
| 3 | 大文件读写 | 10MB 二进制文件 + MD5 校验 |
| 4 | 批量小文件 | 100 个 shard 文件创建 + 读取验证 |
| 5 | 文件元数据 | stat, chmod, utime |
| 6 | 文件管理 | rename, symlink, readlink, remove |
| 7 | JSON 配置读写 | json.dump / json.load 往返验证 |
| 8 | Pickle checkpoint | 模拟 PyTorch checkpoint 序列化/反序列化 |
| 9 | Seek 随机读取 | seek + read + tell 验证 |
| 10 | 清理 | shutil.rmtree 递归删除 |

## 设计参考

本项目参考了 [HashMeta](https://github.com/kurodes/hashmeta) 的 `libfs_syscall.so` 实现模式：
- 相同的 `libsyscall_intercept` hook 机制
- 相同的虚拟 FD 起始地址（`0x4197`）
- 相同的路径前缀过滤 + FD 范围过滤的两阶段过滤设计

区别在于后端：HashMeta 通过 eRPC 转发到分布式元数据服务，本项目通过 CGo 直接调用 JuiceFS VFS 层（无 RPC 开销）。
