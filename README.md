# file-server

[![release](https://img.shields.io/github/v/release/lvv123/file-server)](https://github.com/lvv123/file-server/releases)
[![license](https://img.shields.io/github/license/lvv123/file-server)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)](#快速开始)

![文件列表界面](docs/screenshot.png)

个人/小团队内网文件服务器：**单个可执行文件，服务一个目录**。默认服务当前工作目录，用法接近 `python -m http.server 8007`；数据库、上传临时文件和配置都收进目录下的隐藏文件夹 `.file-server/`，业务目录保持干净。

- 分片上传 + 断点续传，浏览器端保持 4 个分片并发
- 流式下载 + HTTP Range，内存占用与文件大小无关
- SQLite 索引的文件名搜索、全量审计日志、仅本机可访问的管理后台
- 手机、平板浏览器可直接使用，窄屏下自动切换为卡片式布局

> [!WARNING]
> 本项目**没有用户认证，也不提供 HTTPS**，默认监听 `0.0.0.0:8080`。
> 它只适合部署在可信内网（家庭 / 办公室局域网）。**请勿暴露到公网**。
> 服务目录里的所有文件（包括源码、`.env`、`.git`）都可能被下载、修改或删除，请只把需要共享的目录作为服务目录。
> 若确需公网访问，请在反向代理层自行加上认证与 TLS。

## v2.0.0 主要变化

- 目录布局改为「服务目录 + 隐藏的 `.file-server/`」，不再创建或使用 `data/files`。
- 默认服务当前工作目录，`file-server.exe 8007` 即可启动；也可用 `-d D:\share` 指定目录。
- 端口、监听地址、分片大小、上传会话、审计、线程、超时、搜索限制等参数都可通过命令行覆盖。
- 配置文件默认位于 `<服务目录>/.file-server/config.json`，首次运行自动生成。
- `.file-server` 对 Web 端完全隐藏，直接访问返回 `403 RESERVED_PATH`。

从 v1 升级时，`data_root` 配置键已移除；旧的 `data/files` 内容需要移动到新的服务目录，或用 `-d <旧数据目录>/files` 直接服务旧文件，详见下文「v1 迁移」。

## 快速开始

**方式一：下载预编译版本（Windows x64）**

从 [Releases](https://github.com/lvv123/file-server/releases) 下载 `file-server-2.0.0-win64.zip`，解压后直接运行：

```bat
file-server.exe
```

它会把**当前目录**作为服务目录，并在其中创建 `.file-server/`。也可以指定目录和端口：

```bat
file-server.exe 8007
file-server.exe -d D:\share 8007
```

然后浏览器打开 <http://127.0.0.1:8080/>（或你指定的端口）。

exe 静态链接了 CRT，不依赖 VC++ 运行库，可拷到其他 64 位 Windows 机器直接用。

**方式二：从源码构建**

所有依赖已内置在 `third_party/`（cpp-httplib、nlohmann/json、SQLite amalgamation），构建过程**无需联网、无需包管理器**。前提：CMake ≥ 3.20、C++17 编译器。

**Windows（VS2022）**

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
build\Release\file-server.exe -d D:\share 8007
```

**Linux（g++ ≥ 9 / clang ≥ 10）**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/file-server -d /srv/share 8007
```

## 运行

```text
file-server [PORT] [options]
```

| 参数 | 说明 |
| --- | --- |
| `PORT` | 位置参数形式的端口，等价于 `--port PORT` |
| `-d`, `--dir`, `--directory <path>` | 服务目录，默认当前工作目录 |
| `-p`, `--port <port>` | 监听端口，默认 `8080` |
| `-b`, `--bind`, `--bind-address <addr>` | 监听地址，默认 `0.0.0.0`；只允许本机用 `127.0.0.1` |
| `-c`, `--config <path>` | 配置文件，默认 `<dir>/.file-server/config.json` |
| `--chunk-size-mb <n>` | 分片大小，默认 `8` |
| `--session-ttl-hours <n>` | 上传会话保留时长，默认 `24` |
| `--audit-retention-days <n>` | 审计日志保留天数，默认 `30`；`0` 表示不按天数清理 |
| `--audit-max-size-mb <n>` | 审计库容量上限，默认 `512`；`0` 表示不按容量清理 |
| `--admin-allow-ip <ip>` | 允许访问管理接口的 IP，可重复；一旦指定会替换配置文件中的列表 |
| `--thread-min <n>` / `--thread-max <n>` / `--thread-max-queue <n>` | 工作线程配置 |
| `--io-timeout-sec <n>` | 读写超时，默认 `300` |
| `--max-json-body-bytes <n>` | JSON 请求体上限，默认 `1048576` |
| `--search-limit-default <n>` / `--search-limit-max <n>` | 搜索分页限制 |
| `--check-config` | 校验配置、打印最终生效的路径和参数，不创建任何文件 |
| `-h`, `--help` | 显示帮助 |
| `--version` | 显示版本 |

优先级：**内置默认值 < `config.json` < 命令行参数**。

退出码：`0` 正常退出，`1` 启动失败（端口占用、目录不可写、跨卷等），`2` 参数或配置文件错误。

### 目录布局

```text
D:\share\                         <- 服务目录（默认当前目录）
├── .file-server\
│   ├── config.json               <- 首次运行自动生成
│   ├── meta.db                   <- SQLite：文件索引、上传会话、审计日志
│   ├── meta.db-wal
│   ├── meta.db-shm
│   ├── server.lock               <- 单实例锁
│   └── tmp\                      <- 上传中的分片
│       └── <session-id>\*.part
├── 现有文件...
└── 现有目录...
```

- `.file-server/` 在 Web 端**永远不可见**：列表、搜索、下载、删除、重命名、新建目录、上传都会拒绝该路径，符号链接别名也会被拦截。
- 根目录下的 `files/` 不再是特殊目录。它就是普通子目录，和你的其他文件一样被服务。
- 同一目录只允许运行一个 `file-server` 实例；第二个实例会因 `server.lock` 启动失败。
- `tmp/` 与业务文件位于同一磁盘卷，这是合并时原子替换的前提。`.file-server` 不允许是指向其他卷的符号链接/junction。

## 配置

默认配置文件是 `<服务目录>/.file-server/config.json`。首次运行且该文件不存在时，程序会自动生成一份默认配置；显式用 `-c` 指定的文件不存在则报错，不会自动创建。

所有键都可选，缺失的键使用默认值：

```json
{
  "port": 8080,
  "bind_address": "0.0.0.0",
  "chunk_size_mb": 8,
  "session_ttl_hours": 24,
  "audit_retention_days": 30,
  "audit_max_size_mb": 512,
  "admin_allow_ips": ["127.0.0.1"],
  "thread_min": 16,
  "thread_max": 32,
  "thread_max_queue": 64,
  "io_timeout_sec": 300,
  "max_json_body_bytes": 1048576,
  "search_limit_default": 200,
  "search_limit_max": 1000
}
```

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `port` | `8080` | 监听端口，1–65535 |
| `bind_address` | `"0.0.0.0"` | 监听地址 |
| `chunk_size_mb` | `8` | 分片大小，1–1024 |
| `session_ttl_hours` | `24` | 上传会话保留时长 |
| `audit_retention_days` | `30` | 审计日志保留天数，`0` 表示不按天数清理 |
| `audit_max_size_mb` | `512` | 审计库容量上限，`0` 表示不按容量清理 |
| `admin_allow_ips` | `["127.0.0.1"]` | 允许访问管理接口的来源 IP |
| `thread_min` / `thread_max` / `thread_max_queue` | `16` / `32` / `64` | 工作线程 |
| `io_timeout_sec` | `300` | 读写超时 |
| `max_json_body_bytes` | `1048576` | JSON 请求体上限 |
| `search_limit_default` / `search_limit_max` | `200` / `1000` | 搜索分页限制 |

**v1 迁移**：`data_root` 键已移除，配置文件里出现它会直接报错。v1 的 `data/files` 内容需要移动到新的服务目录，或用 `-d <旧数据目录>/files` 直接服务旧文件；旧 `meta.db` 不复用，启动时会重新索引。

外部新增、删除或重命名的文件不会实时出现在列表和搜索中。启动时会全量扫描一次；运行期间可点击页面上的「同步索引」或调用 `GET /api/resync`。

## API

所有接口返回统一信封：

```json
{ "ok": true,  "data": { } }
{ "ok": false, "error": { "code": "NOT_FOUND", "message": "..." } }
```

业务接口无认证，供内网使用；管理接口仅本机可访问。

### 业务接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/files?path=/` | 列出目录内容 |
| `GET` | `/api/search?q=&limit=&offset=` | 按文件名搜索 |
| `POST` | `/api/mkdir` | 新建目录，body `{"path":"/a/b"}` |
| `POST` | `/api/rename` | 重命名，body `{"from":"/a","to":"/b"}` |
| `DELETE` | `/api/files?path=/a` | 删除文件或空目录 |
| `GET` | `/api/resync` | 重新扫描服务目录，让索引与磁盘对齐 |
| `GET` | `/api/download?path=/a/b.bin` | 下载，支持 `Range` |
| `POST` | `/api/upload/init` | 开始上传，body `{"path":"/a/b.bin","total_size":123}` |
| `PUT` | `/api/upload/chunk` | 上传分片，头 `X-Session-Id`、`X-Chunk-Index`，body 为原始字节 |
| `GET` | `/api/upload/session?id=` | 查询会话已接收的分片索引 |
| `POST` | `/api/upload/complete` | 合并分片并落盘，body `{"session_id":"..."}` |

访问 `/.file-server` 及其子路径统一返回 `403 RESERVED_PATH`。

### 上传流程

1. `POST /api/upload/init` → 返回 `{"session_id","chunk_size","total_chunks","expires_at"}`
2. 重复 `PUT /api/upload/chunk`，`X-Chunk-Index` 从 `0` 开始，`Content-Length` 必须**精确等于**该片应有的字节数
3. 中断后 `GET /api/upload/session?id=` 取已接收索引，跳过已完成的分片续传
4. `POST /api/upload/complete` 合并写入目标路径

分片写入 `.file-server/tmp/<session_id>/NNNNNNNN.part`。完成时按索引顺序流式拼接、校验总字节数与声明一致、`fsync`，最后原子替换目标路径。合并失败会保留会话与分片，可重试。

零字节文件同样支持：`total_size` 填 `0`，服务端返回 `total_chunks: 0`，直接调用 `complete` 即可生成空文件。

### 下载与 Range

支持 `bytes=0-1023`（闭区间）、`bytes=1048576-`（到文件末尾）、`bytes=-1024`（最后 1024 字节）三种形式。多区间请求、起点越界、区间非法一律返回 `416` 并带 `Content-Range: bytes */<size>`。

### 管理接口（仅本机）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/admin/status` | 磁盘容量、文件数、活跃会话、审计条数 |
| `GET` | `/admin/audit?limit=&offset=` | 审计记录，`limit` 默认 50、上限 500 |
| `DELETE` | `/admin/files?path=/a` | 删除文件或空目录 |

管理接口按 **TCP 对端地址** 判定来源，不信任 `X-Forwarded-For`。经反向代理访问时管理接口会返回 `403`，应把 `admin_allow_ips` 配成代理主机地址。

### 状态码

| 码 | 含义 |
| --- | --- |
| `200` / `206` | 成功 / 部分内容 |
| `400` | 参数缺失或非法、JSON 错误、分片索引越界 |
| `403` | 路径穿越、访问 `.file-server`、管理接口非本机 |
| `404` | 文件、目录或会话不存在 |
| `409` | 目标已存在、目录非空、分片不齐 |
| `411` | 缺少 `Content-Length` |
| `413` | 分片大小与声明不符、请求体过大 |
| `416` | `Range` 非法 |
| `500` | 磁盘 IO 失败或未预期错误 |
| `507` | 磁盘空间不足 |

## 部署

Linux 用 systemd：

```ini
[Unit]
Description=file-server
After=network.target

[Service]
Type=simple
WorkingDirectory=/srv/share
ExecStart=/srv/file-server/file-server -d /srv/share 8080
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Windows 用计划任务（项目本身不是 Windows 服务）：

```bat
schtasks /create /tn file-server /tr "\"C:\file-server\file-server.exe\" -d \"C:\share\" 8080" ^
  /sc onstart /ru SYSTEM /rl HIGHEST
```

经 nginx 反向代理时注意两点：`client_max_body_size` 必须不小于 `chunk_size`，且建议 `proxy_request_buffering off`，否则 nginx 会把每个分片先缓冲到自己的磁盘上。

## 数据与备份

`.file-server/meta.db` 中的 `files` 索引在每次启动时按磁盘实际内容校对，**损坏或丢失可以重建**：删掉 `meta.db` 重启，或启动后调用 `GET /api/resync` 即可恢复列表与搜索。审计日志也在 `meta.db` 里，删除会一起丢失。

备份时停掉服务后直接复制整个服务目录即可；只想备份文件内容则复制业务文件，索引可以靠重扫恢复。

## 测试

单元测试（零第三方依赖的自研断言框架，118 个用例 / 680 条断言）：

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure

:: 也可直接运行，支持 --filter <子串> / --list
build\Debug\fs_unit_tests.exe --filter range
```

```sh
# Linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

接口 / 集成测试（会自行启动真实服务进程，使用临时目录，不碰你的数据）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/api/run_api_tests.ps1
```

常用参数：`-Only <子串>` 只跑匹配的用例，`-KeepData` 保留临时目录，`-Port` 指定端口（默认自动选空闲端口），`-DiskFullDir <盘符>\` 打开磁盘写满用例。当前结果：69 项通过、1 项跳过（磁盘满，需要一块接近写满的卷）。

大文件验收（写入约 2 倍于文件大小的磁盘空间，默认 10 GiB，耗时数分钟）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/api/acceptance_large.ps1
# 快速版本
powershell -NoProfile -ExecutionPolicy Bypass -File tests/api/acceptance_large.ps1 -SizeMB 64
```

该脚本完整走一遍「上传 → 强杀进程 → 重启 → 查询会话 → 续传 → 合并 → 校验文件内容」。

## 开发

```text
src/            服务端源码（C++17）
  main.cpp        入口、启动序列、维护线程
  cli.cpp         命令行解析
  layout.cpp      服务目录与 .file-server 布局
  server.cpp      路由注册
  handler_*.cpp   各接口实现
  repo_*.cpp      SQLite 数据访问
  upload.cpp      分片落盘与合并
  download.cpp    流式下载与 Range
  paths.cpp       路径规范化、保留路径与越界防护
web/            内嵌前端（原生 HTML/CSS/JS，无构建步骤）
tests/unit/     单元测试
tests/api/      接口测试脚本（PowerShell）
third_party/    依赖源码
tools/          gen_assets.py：把 web/ 转成 C++ 字符串
```

前端资源通过 `tools/gen_assets.py` 转换为 `src/static_assets.cpp` 编入二进制。修改 `web/` 后重新生成：

```sh
python tools/gen_assets.py
```

或配置 `-DFS_REGENERATE_ASSETS=ON` 在构建时自动执行（需要 Python）。

架构约束：每个线程使用独立的 SQLite 连接并强制 `WAL`、`busy_timeout`、`synchronous=NORMAL`；所有用户路径经规范化、根目录包含性检查与 `.file-server` 保留路径检查；分片与下载全程流式，不把完整文件或大分片读入内存。

浏览器要求：任何 2018 年之后的现代浏览器（用到 `fetch`、`localStorage`、`async/await`），不兼容 IE。

## 第三方组件

| 组件 | 版本 | 许可 |
| --- | --- | --- |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.18.3 | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT |
| [SQLite](https://sqlite.org/) | 3.50.4 amalgamation | Public Domain |

三者均以源码形式放在 `third_party/` 并静态编入可执行文件，各自的许可证条款随源码保留。

## 许可证

[MIT](LICENSE)
