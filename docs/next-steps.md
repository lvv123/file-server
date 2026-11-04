# 交接说明：后续工作与不变量

> 写于 2026-11-04，对应提交 `016010c`。本文档用于在新会话中继续开发前恢复上下文。

## 一、当前状态

| 项 | 状态 |
| --- | --- |
| HEAD | `016010c fix: address the code audit findings`，已推送 `origin/main` |
| 构建（MSVC 2022, `/W4`） | 0 error / 0 warning |
| 单元测试 | 80 个用例 / 532 条断言，全通过 |
| 接口测试 | 66 通过 / 0 失败 / 1 跳过（IT-Q1 磁盘满，需 `-DiskFullDir`） |
| 大文件验收 | 10 GiB「上传→强杀→重启→续传→合并→校验」通过 |
| Linux | 代码跨平台，但只在 WSL 里装过工具链，**未做过完整验证**（用户明确暂不需要） |
| Release | `v1.0.0` 已发布，但**早于移动端适配与审计修复**，内容已过时 |

## 二、构建与验证命令

```powershell
# 编译前必须先杀掉占用 exe 的进程，否则 LNK1168
Get-Process file-server -ErrorAction SilentlyContinue | Stop-Process -Force

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel

# 单元测试（PowerShell 管道会乱序，必须用 cmd /c 重定向）
cmd /c ".\build\Debug\fs_unit_tests.exe > ut.log 2>&1"

# 接口测试（会自己拉起临时实例：随机端口 + %TEMP%\fsrv-api-*，跑完自清理）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\api\run_api_tests.ps1
# 只跑某组：-Only "IT-R"

# 大文件验收（默认 10 GiB，注意会写入约 2 倍空间）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\api\acceptance_large.ps1 -SizeMB 4096
```

改了 `web/` 下的任何文件后必须重新生成内嵌资源并重编：

```powershell
python tools\gen_assets.py
cmake --build build --config Debug --parallel
```

## 三、待办 1（推荐先做）：`upload_chunk` 的 RAII 与拆分 —— 审计第 5/9 条

**位置**：`src/upload.cpp:260-384`（`upload_chunk`，约 125 行）

**问题**：6 条早退分支各自手写 `std::fclose` + `fs::remove(part_tmp)` + `throw`，顺序还略有差异。这是全项目风险最高的函数（不可信字节流 + 磁盘 I/O + 持久化语义），历史上死锁、锁模型、空间预检、`stat` 检查等问题都出在这里。新增一条错误路径就可能漏掉 `fclose` 或留下 `.tmp` 垃圾。

**做法**：

1. 先加一个守卫，把资源清理从控制流里摘出来：

```cpp
struct PartFile {
  std::FILE* f = nullptr;
  fs::path tmp;
  bool committed = false;
  ~PartFile() {
    if (f) std::fclose(f);
    if (!committed) { std::error_code ec; fs::remove(tmp, ec); }
  }
};
```

2. 再把函数拆成 `validate_request` / `stream_to_part` / `commit_part` 三段，最后单独更新位图。拆分是清理的顺带结果，不要为拆而拆。

**风险点（务必保持等价）**：

- 清理顺序必须是「先 `fclose` 再 `remove`」（Windows 上打开中的文件删不掉）。
- `errno` 必须在 `fclose` **之前**保存，否则被覆盖。
- 失败后必须仍按 `BodyGate` 语义把请求体读空，否则 keep-alive 连接上后续请求会串包。

**验证**：单元测试 + 全量接口测试 + `acceptance_large.ps1 -SizeMB 4096`。预计半天。

## 四、待办 2（条件性）：`BodySource` 抽象 + `upload_chunk` 单测 —— 审计第 7 条

**现状**：依赖链 `upload.h → body_gate.h → hpx.h → httplib.h`，导致 `upload_chunk` **一个单元测试都没有**（必须构造 `httplib::ContentReader` 才能测）。

**判断标准**：只做抽象不补单测 = 不做；抽象 + 8～10 个单测 = 做。预计一天。

**做法**：在 `upload.h` 里定义与框架无关的接口，`BodyGate` 移到 handler 层去实现它：

```cpp
struct BodySource {
  virtual bool read(const std::function<bool(const char*, size_t)>& sink) = 0;
  virtual void drain() = 0;
  virtual void abandon() = 0;
  virtual ~BodySource() = default;
};
```

**应当补的单测**：长度与声明不符 → 413；写盘失败 → 507 且不留下 `.part`；重复索引覆盖；位图中途更新；零字节会话；`abandon()` 后不再读。

## 五、明确不做：审计第 8 条（全局会话锁表争用）

`session_locks()` 里的全局锁只持有一次哈希查找 + `shared_ptr` 拷贝的时长，真正耗时的 `fsync`/写盘在会话 `io` 锁里。实测 4 路并发 1.3 GB/s，瓶颈在磁盘。分桶在百人内网场景下没有可测收益，反而多一层间接。**除非 profiling 显示它排到前面，否则不要动。**

## 六、关键不变量（改动时不能破坏）

1. **分片请求体必须始终读完**。任何失败路径都要把 body 排空，否则同一 keep-alive 连接上的下一个请求会读到上一请求的残留字节。
2. **没有 `Content-Length` 时绝对不能 `drain()`**。cpp-httplib 会 `recv()` 直到对端关闭 → 与客户端互锁死等（读超时 300s）。这种情况用 `BodyGate::abandon()`。
3. **合并必须「写临时文件 → `fsync` → 原子替换」**，任何失败都保留会话与分片，允许重试。
4. **所有用户路径必须规范化 + 根目录包含性检查**，不能只靠字符串里找 `..`。
5. **每个线程独立 SQLite 连接**，打开时强制 `WAL` / `busy_timeout=5000` / `synchronous=NORMAL` / `foreign_keys=ON`；连接不得跨线程共享。
6. **下载全程流式**，不得整文件读入内存；Range 手动解析，并在 `download_file` 开头清空 `req.ranges`（否则框架会二次切片）。
7. **失败操作也要写审计**（`result=fail`），审计写失败只能打日志，不得影响主流程。
8. **`tmp/` 与 `files/` 必须同一挂载点**，启动时强制校验。

## 七、已知的坑（都实际踩过）

| 坑 | 说明 |
| --- | --- |
| `MoveFileExW` 不能替换被打开的文件 | 即使对方给了 `FILE_SHARE_DELETE` 也报 `ACCESS_DENIED`，且是**永久**拒绝。必须回退 `ReplaceFileW(dst, src, nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)`（已在 `fsops.cpp` 实现） |
| PowerShell 里 `curl.exe -d '{...}'` | 引号会被 PowerShell 吃掉，导致服务端 `INVALID_JSON`。写成文件用 `-d @body.json` |
| 无头 Edge 有约 500px 最小窗口宽度 | 想验证 390px 布局，要用 iframe 包装页（外层窗口 400px，iframe 内 390px） |
| PowerShell 单元素数组解包 | `@($x) \| Where-Object {...}` 单元素时 `.Count` 抛异常，要写成 `@(@($x) \| Where-Object {...})` |
| 控制台中文文件名乱码 | 文件本身是合法 UTF-8，只是代码页问题，不要误判为损坏 |
| `fs::xxx(..., ec)` 之后用 `errno` | `errno` 是陈旧值，必须用 `io_error_from(what, ec)` |
| 接口测试用例互相污染 | 断言目录是否为空时，用独立目录，别和其他用例共用父子目录 |
| 构建报 `LNK1163 COMDAT` | 删掉 `build\file-server.dir` 下的 `*.obj` 重新链接 |

## 八、可选：发 v1.1.0

当前 HEAD 含移动端适配与审计修复，但 Release 仍是 `v1.0.0`。若要发：

```powershell
cmake --build build --config Release --parallel
# 打包 exe + config.json + README.md + LICENSE 为 zip，然后
gh release create v1.1.0 <zip> --title "v1.1.0" --notes-file <notes.md>
```

注意 Release 配置已静态链接 MSVC 运行时（`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded`），打包后不依赖 `MSVCP140.dll`。
