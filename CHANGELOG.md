# Changelog

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

---

## [未发布]

### 新增

**动态库与 Runtime 注入（2026-11）**

- `RTSPMECH_LIBRARY_TYPE=STATIC|SHARED` + `RTSPMECH_API` 导出宏；静态链入的
  netmech / picohttpparser 符号用 `-Wl,--exclude-libs,ALL` 隐藏；CI 增加 Shared job
- `RtspServerConfig` / `RtspConnectorConfig` / `RtspClient::Config` 增加
  `std::shared_ptr<netmech::Runtime> runtime`：io 线程数由应用决定
- netmech submodule 升级到支持 `Mechanism` Runtime 重载的版本

### 修复（2026-11 审查）

- Builder 请求行/状态行的 CRLF 注入（method/uri/reason）
- `RtspConnector::close()` 取消语义：进行中的 connect 之后不再建会话
- Transport 参数名大小写不敏感；`Content-Length` 容忍冒号前空白
- `on_ready` 移到 server 记账之后（`session_count()` 能数到自己）
- `parse_url` 支持 IPv6 字面量；`extract_controls` 按行匹配
- server 连接表清理过期 weak；`send_request` 失败不消耗 CSeq


### 新增

**CMake 安装与导出（find_package）**

- `cmake --install` 现在装出完整的一套：头文件、静态库、`lib/cmake/rtspmech/` 下的
  `rtspmechConfig.cmake` / `rtspmechConfigVersion.cmake` / `rtspmechTargets.cmake`，
  以及 `share/doc/rtspmech` 下的 LICENSE / NOTICE
- 使用者只需要两行：
  ```cmake
  find_package(rtspmech REQUIRED)
  target_link_libraries(app PRIVATE rtspmech::rtspmech)
  ```
- Config 里 `find_dependency(netmech)`；vendored 的 picohttpparser 以
  `$<LINK_ONLY:>` 形式随 target 导出，使用者不用单独链

- 默认安装前缀：Linux `/usr/local`，Windows `C:/Program Files/rtspmech`

---

## [0.9.0] — 首个版本

RTSP 的**协议胶水层**：TCP 字节流进来，切成消息，通过弱回调抛给应用。
推流/取流、SDP 语义、认证握手都不在本库职责内。

### 新增

**分帧**

- 从 TCP 字节流切出 RTSP 消息：文本（`\r\n\r\n` + `Content-Length`）与
  二进制 `$` 帧（4 字节头：`$` + channel + 16 位长度）混流。
- 单条消息上限 2 MB，超限返回 `RtspErrc::too_large` 并断连接。
- 头部一直不结束也会在上限处断开，不会无限吃内存。

**消息解析**

- 请求行 / 状态行 / 头部（picohttpparser）。
- `RtspRequest` / `RtspResponse` 提供 `header()`（大小写不敏感）、`cseq()`、
  `session()`（自动去掉 `;timeout=`）、`www_authenticate()`。
- 自动判断请求还是响应。

**会话**

- `RtspSession` 实现 netmech 的 `ConnHandler`：读循环、分帧、派发一条龙。
- `RtspHandler`：`on_ready` / `on_request` / `on_response` / `on_interleaved` / `on_close`。
- **CSeq 自动递增**：`send_request()` 内部填，builder 里显式设过就不覆盖。
- `send_interleaved()` 发 `$` 帧。

**Transport**

- `parse_transport()` 覆盖 UDP 单播 / 组播 / interleaved 各种写法。
- `interleaved=0` 单值按标准隐含 `ch2 = ch1 + 1`（RFC 2326 §12.39）。
- `is_rtcp()`：按第 2 字节的 PT（200~204）区分 RTP/RTCP，
  分两个 channel 和复用一个 channel 的设备都适用。

### 设计上的两个要点

1. **`RtspSession` 是 `ConnHandler`，不是"接管器"** —— netmech 的处理器创建时绑定，
   不能中途换。所以是在 `on_accept` / `on_linkup` 里创建并交回去。
2. **第一个请求要在 `on_ready` 里发** —— `on_linkup` 阶段连接还没被驱动，
   那时 `send_request()` 是发不出去的。

### 已知限制

- 只支持 RTSP 1.0（RFC 2326）。RTSP 2.0 的管道化、会话超时等特性未实现。
- 不做认证（Basic / Digest），只把 `WWW-Authenticate` 原样交给应用。
- 不解析 SDP 语义，`resp.body` 是原始字节。
- interleaved 模式下 rtpmech 的 `RtpSession` 用不上（它绑的是 UDP）。

### 验证情况

| 平台 | 编译器 | 结果 |
|---|---|---|
| Linux x86_64（WSL2） | GCC 15.2 | 29 用例 / 139 断言全绿，零警告 |
| 示例 `rtsp_demo` | — | OPTIONS → DESCRIBE → SETUP → PLAY 全流程 PASS |
