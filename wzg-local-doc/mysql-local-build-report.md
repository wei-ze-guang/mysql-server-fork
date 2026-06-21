# MySQL 本地首次构建报告

记录时间：2026-06-21  
项目目录：`/Users/macbook/database/mysql-server-fork`  
源码版本：MySQL Server `8.4.10`

## 目标

本次构建的目标是把构建产物、安装产物和运行产物都放在当前项目目录内，避免文件分散到系统路径，同时保留 Ninja 的增量编译能力，方便后续改代码后快速重编。

最终目录约定：

- `_build/mysql-local`：CMake/Ninja 构建目录，保留增量编译缓存。
- `_install/mysql-local`：`cmake --install` 后的安装目录。
- `_runtime/mysql-local`：本地运行数据、socket、pid、日志、临时文件目录。

这三个目录已经加入 `.gitignore`。

## 使用到的工具

本机已有可用工具，本次没有安装系统软件或全局工具。

| 工具 | 实际使用版本/路径 | 用途 |
| --- | --- | --- |
| CMake | `cmake version 4.3.3` | 生成 Ninja 构建系统、执行安装 |
| Ninja | `1.12.1` | 编译 MySQL |
| Apple Clang | `Apple clang version 17.0.0` | C/C++ 编译器 |
| Bison | `/usr/local/opt/bison/bin/bison`，`GNU Bison 3.8.2` | 生成解析器 |
| OpenSSL | `/usr/local/opt/openssl@3`，`OpenSSL 3.6.2` | SSL/TLS 依赖 |

注意：系统自带 `/usr/bin/bison` 版本较旧，不适合这次构建，所以显式使用了 `/usr/local/opt/bison/bin/bison`。

## 配置过程

从项目根目录执行：

```bash
cmake -S . -B _build/mysql-local -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PWD/_install/mysql-local" \
  -DWITH_SSL=/usr/local/opt/openssl@3 \
  -DBISON_EXECUTABLE=/usr/local/opt/bison/bin/bison
```

配置含义：

- `-S .`：源码目录是当前项目。
- `-B _build/mysql-local`：构建缓存和中间产物放在项目内。
- `-G Ninja`：使用 Ninja，便于快速增量编译。
- `RelWithDebInfo`：带调试信息的优化构建，适合本地开发调试。
- `CMAKE_INSTALL_PREFIX`：安装产物放进 `_install/mysql-local`。
- `WITH_SSL` 和 `BISON_EXECUTABLE`：固定使用本机可用且版本合适的依赖路径。

## 编译过程

本机 CPU 核心较多，但没有使用最大线程数，保留资源给系统：

```bash
cmake --build _build/mysql-local --parallel 8
```

本次完整默认目标编译成功。

验证构建目录里的 `mysqld`：

```bash
_build/mysql-local/runtime_output_directory/mysqld --version
```

输出确认：

```text
mysqld  Ver 8.4.10 for macos15.7 on x86_64 (Source distribution)
```

## 安装过程

安装到项目内目录：

```bash
cmake --install _build/mysql-local --prefix "$PWD/_install/mysql-local"
```

安装后验证：

```bash
_install/mysql-local/bin/mysqld --version
_install/mysql-local/bin/mysql --version
```

两者均显示 `8.4.10`。

## 运行目录初始化

创建运行目录：

```bash
mkdir -p _runtime/mysql-local/{data,logs,tmp,run}
```

初始化数据目录：

```bash
_install/mysql-local/bin/mysqld \
  --initialize-insecure \
  --basedir="$PWD/_install/mysql-local" \
  --datadir="$PWD/_runtime/mysql-local/data" \
  --log-error="$PWD/_runtime/mysql-local/logs/initialize.err"
```

说明：`--initialize-insecure` 会创建无密码的本地 `root@localhost`，只适合本机开发验证。

## 启动与验证

推荐使用 `mysqld --daemonize` 启动后台服务：

```bash
_install/mysql-local/bin/mysqld --daemonize \
  --basedir="$PWD/_install/mysql-local" \
  --datadir="$PWD/_runtime/mysql-local/data" \
  --socket="$PWD/_runtime/mysql-local/run/mysql.sock" \
  --pid-file="$PWD/_runtime/mysql-local/run/mysqld.pid" \
  --log-error="$PWD/_runtime/mysql-local/logs/mysqld.err" \
  --tmpdir="$PWD/_runtime/mysql-local/tmp" \
  --port=3307 \
  --mysqlx=OFF
```

连接验证：

```bash
_install/mysql-local/bin/mysql \
  --protocol=SOCKET \
  --socket="$PWD/_runtime/mysql-local/run/mysql.sock" \
  -uroot \
  -e "SELECT VERSION() AS version, @@port AS port, @@datadir AS datadir;"
```

本次验证结果：

```text
version  port  datadir
8.4.10   3307  /Users/macbook/database/mysql-server-fork/_runtime/mysql-local/data/
```

当前实例使用：

- TCP 端口：`3307`
- Unix socket：`_runtime/mysql-local/run/mysql.sock`
- pid 文件：`_runtime/mysql-local/run/mysqld.pid`
- 错误日志：`_runtime/mysql-local/logs/mysqld.err`

## 停止服务

```bash
_install/mysql-local/bin/mysqladmin \
  --protocol=SOCKET \
  --socket="$PWD/_runtime/mysql-local/run/mysql.sock" \
  -uroot shutdown
```

## 后续增量编译

以后改代码后继续使用同一个构建目录即可增量编译：

```bash
cmake --build _build/mysql-local --parallel 8
```

如果只想重编 `mysqld`：

```bash
cmake --build _build/mysql-local --parallel 8 --target mysqld
```

不要删除 `_build/mysql-local`，否则会丢失 CMake/Ninja 的增量状态。

## 本次注意事项

- 构建和运行产物都保存在当前项目内，没有放到系统目录。
- 没有执行 `brew install`、`npm install -g`、`curl | sh` 等系统环境修改命令。
- 曾尝试用 shell 后台方式启动 `mysqld`，短生命周期 shell 退出后进程没有稳定保留；改用 `mysqld --daemonize` 后启动正常。
- macOS 当前文件系统大小写不敏感，MySQL 日志中会提示设置 `lower_case_table_names=2`，这是预期现象。
- 初始化使用的是 insecure root，本地开发方便，但不适合作为生产配置。
