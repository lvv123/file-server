# file-server

个人/小团队内网文件服务器。单个可执行文件 + 一个 JSON 配置 + 一个数据目录即可运行，面向百人以内、约 1T 存储、单文件约 10G 的场景。

浏览器打开 `http://<主机>:8080/` 即为 Web 客户端。

## 特性

- **分片上传 + 断点续传**：分片大小可配置（内置默认 8MB，随附的 `config.json` 使用 16MB），每片一个独立 HTTP 请求、请求体为原始字节；浏览器端同时保持 4 个分片在途，上传中断后可通过会话恢复。
- **流式下载 + HTTP Range**：手动解析 `Range` 并返回 `206`/`Content-Range`，全程流式，不受文件大小影响内存占用。
- **文件/目录管理**：列表、新建多级目录、重命名（文件与目录）、删除、覆盖上传（临时文件合并后原子替换）。
- **全库文件名搜索**：基于 SQLite 索引的模糊匹配。
- **审计日志**：记录时间、客户端 IP、操作类型、路径、大小、结果、错误、耗时、User-Agent；成功与失败都记录，按天数和容量滚动清理。
- **管理后台**：状态页（磁盘容量、文件数、活跃会话、审计条数）、审计查询、删除；**仅本机可访问**。
- **可靠性**：上传先写 `tmp/` 再原子移动到 `files/`；启动时校验 `tmp/` 与 `files/` 在同一挂载点，不满足拒绝启动；崩溃后会话与分片仍在，可继续上传。

明确不做：秒传/去重、限速、用户认证、全文搜索、HTTPS、WebSocket、回收站、CLI/SDK。

## 构建

依赖已内置在 `third_party/`（cpp-httplib、nlohmann/json、SQLite amalgamation），无需联网、无需包管理器。

**Windows（VS2022）**

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

产物：`build/Release/file-server.exe`

**Linux（g++ ≥ 9 / clang ≥ 10）**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel -j
```

产物：`build/file-server`

## 运行

```sh
./file-server -c config.json
```

首次运行会在 `data_root` 下自动创建 `files/`、`tmp/`、`meta.db`。

命令行参数：

| 参数 | 说明 |
| --- | --- |
| `-c`, `--config <path>` | 配置文件路径，默认 `config.json` |
| `--check-config` | 只校验配置并初始化目录，不启动服务 |
| `-h`, `--help` | 显示帮助 |
| `--version` | 显示版本 |

退出码：`0` 正常，`1` 启动失败（配置以外的原因），`2` 配置错误。

## 配置

配置文件为 JSON，缺失的键使用默认值。

```json
{
  "port": 8080,
  "bind_address": "0.0.0.0",
  "data_root": "./data",
  "chunk_size_mb": 16,
  "session_ttl_hours": 24,
  "audit_retention_days": 30,
  "audit_max_size_mb": 512,
  "admin_allow_ips": ["127.0.0.1"]
}
```

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `port` | `8080` | 监听端口，1–65535 |
| `bind_address` | `"0.0.0.0"` | 监听地址 |
| `data_root` | `"./data"` | 数据目录，包含 `files/`、`tmp/`、`meta.db` |
| `chunk_size_mb` | `8` | 分片大小，1–1024（随附配置用 16） |
| `session_ttl_hours` | `24` | 上传会话保留时长 |
| `audit_retention_days` | `30` | 审计日志保留天数，`0` 表示不按天数清理 |
| `audit_max_size_mb` | `512` | 审计库容量上限，`0` 表示不按容量清理 |
| `admin_allow_ips` | `["127.0.0.1"]` | 允许访问管理接口的来源 IP |

可选高级键：`thread_min`(16)、`thread_max`(32)、`thread_max_queue`(64)、`io_timeout_sec`(300)、`max_json_body_bytes`、`search_limit_default`(200)、`search_limit_max`(1000)。

> `tmp/` 与 `files/` 必须位于同一挂载点（同一磁盘卷），否则启动直接失败并退出。
>
> 管理接口按 TCP 对端地址判定来源，不信任 `X-Forwarded-For`。通过反向代理访问时，管理接口会对代理后的来源返回 `403`，此时应把 `admin_allow_ips` 配置为代理主机地址。

## API

统一响应体：

```json
{ "ok": true, "data": { } }
{ "ok": false, "error": { "code": "NOT_FOUND", "message": "..." } }
```

### 业务接口（内网，无认证）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/files?path=/` | 列出目录内容 |
| `GET` | `/api/search?q=&limit=&offset=` | 按文件名搜索 |
| `POST` | `/api/mkdir` | 新建目录，body `{"path":"/a/b"}` |
| `POST` | `/api/rename` | 重命名，body `{"from":"/a","to":"/b"}` |
| `DELETE` | `/api/files?path=/a` | 删除文件或空目录 |
| `GET` | `/api/resync` | 重新扫描 `files/`，让索引与磁盘对齐 |
| `GET` | `/api/download?path=/a/b.bin` | 下载，支持 `Range` |
| `POST` | `/api/upload/init` | 开始上传，body `{"path":"/a/b.bin","total_size":123}` |
| `PUT` | `/api/upload/chunk` | 上传分片，头 `X-Session-Id`、`X-Chunk-Index`，body 为原始字节 |
| `GET` | `/api/upload/session?id=` | 查询会话已接收分片 |
| `POST` | `/api/upload/complete` | 合并分片，body `{"session_id":"..."}` |

`/api/resync` 是唯一对全库生效的维护接口：它递归遍历 `files/`，把磁盘上新增的条目补进索引、把已消失的条目删掉，返回 `{"scanned","inserted","removed","file_count"}`。页面上的「同步索引」按钮就是它。用 `GET` 而不是 `POST`，是因为无 body 的 `POST` 会让 cpp-httplib 等到对端关闭连接才开始处理；响应带 `Cache-Control: no-store`。

### 管理接口（仅本机）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/admin/status` | 磁盘容量、文件数、活跃会话、审计条数 |
| `GET` | `/admin/audit?limit=&offset=` | 审计记录 |
| `DELETE` | `/admin/files?path=/a` | 删除文件或空目录 |

### 上传流程

1. `POST /api/upload/init` → `{"session_id","chunk_size","total_chunks","expires_at"}`
2. 重复 `PUT /api/upload/chunk`，`X-Chunk-Index` 从 `0` 开始，每片正好 `chunk_size` 字节（最后一片为余数）
3. 中断后 `GET /api/upload/session?id=` 取已接收索引，跳过已完成的分片续传
4. `POST /api/upload/complete`

分片写入 `tmp/<session_id>/NNNNNNNN.part`（先写唯一命名的临时文件再 `rename`），完成时按序流式拼接、校验总字节数、`fsync`，再原子替换到目标路径。合并失败会保留会话与分片，可重试。

同一会话的多个分片可以并发上传：服务端对会话持共享锁写分片，只在合并/删除时取排他锁，位图更新用独立短锁串行化。浏览器端默认保持 4 个分片在途。

### 状态码

| 码 | 含义 |
| --- | --- |
| `200` / `206` | 成功 / 部分内容 |
| `400` | 参数缺失或非法、JSON 错误、分片索引非法 |
| `403` | 路径穿越、管理接口非本机 |
| `404` | 文件、目录或会话不存在 |
| `409` | 目标已存在、目录非空、分片不齐 |
| `411` | 缺少 `Content-Length` |
| `413` | 分片大小与声明不符、请求体过大 |
| `416` | `Range` 非法 |
| `500` | 磁盘 IO 或未预期错误 |
| `507` | 磁盘空间不足 |

## 数据目录

```
data/
├── files/      # 业务文件，直接映射真实磁盘目录
├── tmp/        # 上传中的分片与会话临时文件（必须与 files/ 同挂载点）
└── meta.db     # SQLite：files 索引、upload_sessions、audit_logs
```

`files/` 下的内容即用户可见的文件树，可以按需备份或迁移；`meta.db` 中的 `files` 表在启动时会按磁盘实际内容做全量校对，损坏或丢失可重建。

## 测试

单元测试（零第三方依赖的自研断言框架）：

```sh
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
# 或直接运行，支持 --filter <子串> / --list
./build/Debug/fs_unit_tests.exe --filter range
```

接口/集成测试（会自行启动真实服务进程，使用临时数据目录）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/api/run_api_tests.ps1
```

常用参数：`-Only <子串>` 只跑匹配的用例，`-KeepData` 保留临时数据目录，`-Port` 指定端口（默认自动选空闲端口）。

大文件验收（写入约 2 倍于文件大小的磁盘空间，默认 10 GiB，耗时数分钟）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/api/acceptance_large.ps1
# 快速版本
powershell -NoProfile -ExecutionPolicy Bypass -File tests/api/acceptance_large.ps1 -SizeMB 64
```

该脚本会完整走一遍「上传 → 强杀进程 → 重启 → 查询会话 → 续传 → 合并 → 校验首尾字节」，对应验收标准第 1 条。

## 开发

```
src/            服务端源码（C++17）
  main.cpp        入口、启动序列、维护线程
  server.cpp      路由注册
  handler_*.cpp   各接口实现
  repo_*.cpp      SQLite 数据访问
  upload.cpp      分片落盘与合并
  download.cpp    流式下载与 Range
  paths.cpp       路径规范化与越界防护
web/            内嵌前端（原生 HTML/CSS/JS，无构建步骤）
tests/unit/     单元测试
tests/api/      接口测试脚本
third_party/    依赖源码
```

前端资源通过 `tools/gen_assets.py` 转换为 `src/static_assets.cpp` 编入二进制。修改 `web/` 后重新生成：

```sh
python tools/gen_assets.py
```

或配置 `-DFS_REGENERATE_ASSETS=ON` 在构建时自动执行（需要 Python）。

架构约束：每个线程使用独立的 SQLite 连接并强制 `WAL`、`busy_timeout`、`synchronous=NORMAL`；所有用户路径经规范化与根目录包含性检查；分片与下载全程流式，不将完整文件或大分片读入内存。
