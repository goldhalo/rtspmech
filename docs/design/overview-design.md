# rtspmech 概要设计

> 状态：讨论稿。
> 接口契约见 [`proposed/`](proposed/)（5 个头）。

---

## 1. 定位与范围

rtspmech 是 **RTSP 的协议胶水层**，不是 RTSP 实现。

| 做 | 不做 |
|---|---|
| 从 TCP 字节流里切出 RTSP 消息（文本 + `$` 帧混流） | 推流、取流（那是应用） |
| 用 picohttpparser 解析请求行 / 状态行 / 头部 | SDP 深度解析（只给原始 body） |
| RTSP 语义辅助：`CSeq` / `Session` / `Transport` / `$` 帧 | Digest / Basic 认证握手 |
| 通过**弱回调**把消息抛给应用 | 会话状态机、重连、保活策略 |

**一句话**：给一块 TCP 连接，它负责"把方言切成句子"，剩下全靠应用。

---

## 2. 为什么是这个定位

用户的原话：

> rtspmech 库本身应该是很简单的，协议解析我们不自己做，有很多开源的 http/rtsp 协议解析库，
> 网络我们有 netmech，剩下的就是那一套弱回调了。至于推流、取流的那是应用了。

所以三层各司其职：

```
┌──────────────────────────────────────┐
│ 应用：DESCRIBE → SETUP → PLAY → 收流  │  ← 会话管理、SDP、认证
├──────────────────────────────────────┤
│ rtspmech：分帧 + 头部解析 + 弱回调     │  ← 本库（很小）
├──────────────────────────────────────┤
│ picohttpparser：请求行/状态行/头部      │  ← thirdparty
├──────────────────────────────────────┤
│ netmech::Conn：TCP 字节流              │  ← 已有
└──────────────────────────────────────┘
```

---

## 3. 公共接口清单

对外 **5 个头**：

| 头 | 内容 |
|---|---|
| `span.hpp` | 轻量 span（C++17 没有 `std::span`） |
| `common.hpp` | `RtspErrc` 错误码 |
| `rtsp_transport.hpp` | `RtspTransport` + `parse_transport()` |
| `session.hpp` | `RtspSession` / `RtspHandler` / `RequestBuilder` / `ResponseBuilder` |
| `rtspmech.hpp` | 汇总头 |

没有内部头——就这么点东西，不值得再分。

---

## 4. 分帧：本库唯一有点难度的地方

### 4.1 为什么难

RTSP 一个 TCP 连接上**混杂两种数据**：

```
OPTIONS rtsp://... RTSP/1.0\r\n      ← 文本
CSeq: 1\r\n
\r\n
$ 0 0x05 0xDC <1436 字节 RTP>         ← 二进制（interleaved）
$ 1 0x00 0x1C <28 字节 RTCP>
RTSP/1.0 200 OK\r\n                  ← 又是文本
CSeq: 1\r\n
\r\n
```

TCP 是字节流，可能任意切开：
- 一条消息分两次到达
- 一次到达包含 3 条消息
- 切在 `\r\n\r\n` 中间、切在 `$` 帧头中间

### 4.2 状态机

```
                    ┌─────────────────────────────┐
                    │  IDLE（等新消息的第一个字节） │
                    └──────────┬──────────────────┘
                 '$'           │           其它
        ┌────────────────┐     │     ┌──────────────────┐
        ▼                │     │     ▼                  │
  ┌───────────┐          │     │  ┌──────────────┐      │
  │ BINARY：   │          │     │  │ TEXT：累积到   │      │
  │ 收 4 字节头│          │     │  │ \r\n\r\n      │      │
  │ 再收 len   │          │     │  └──────┬───────┘      │
  └─────┬─────┘          │     │         │ 头部结束     │
        │ 一帧完整        │     │         ▼              │
        └────────────────┴─────┴── 解析 Content-Length   │
                                  └──────┬───────────────┘
                                         │ 正文收满（或 CL=0）
                                         ▼
                                  抛出消息 → 回 IDLE
```

**关键判断**：流的第一个字节是 `$` 就是二进制帧，否则是文本。

### 4.3 三个边界情况

| 情况 | 处理 |
|---|---|
| `Content-Length` 缺失 | 视为 0（RTSP 里只有带 body 的响应才有这个头） |
| `Content-Length` 巨大（恶意/错误） | 超过 **2 MB** → `RtspErrc::too_large`，断连接 |
| 文本里出现 `$`（比如 SDP 里的字符） | 不会——`$` 只在**消息边界**处才被当成帧头，body 内部不扫描 |

最后一条很重要：**只有"上一条消息结束后"的第一个字节为 `$` 才进入二进制模式**，
body 里的 `$` 不会误判。

### 4.4 缓冲策略

- 接收缓冲按需增长，上限 2 MB（与 `Content-Length` 上限一致）
- 消息解析完成后**不立刻释放**——头部和 body 的 `string_view` 指向它
- 回调返回后才复用/释放（这就是"借用视图"的代价）

---

## 5. Transport 与 interleaved

### 5.1 字段设计

照搬 live555 / ffmpeg 的 `rtsp_header_transport_t`——那套字段把各种设备发来的
组合都覆盖了，见 [`proposed/rtsp_transport.hpp`](proposed/rtsp_transport.hpp)。

### 5.2 `interleaved=0` 的语义

**按 RFC 2326 §12.39 的标准**：

| 写法 | 含义 |
|---|---|
| `interleaved=0-1` | RTP 走通道 0，RTCP 走通道 1 |
| `interleaved=0`（单值） | **隐含 `ch2 = ch1 + 1`** —— RTP 偶数、RTCP 紧随的奇数 |

> 最初以为"单值表示 RTP/RTCP 复用同一个通道"，这是错的。标准里单值就是省略了
> 第二个，语义与 `0-1` 等价。
>
> 但确实有设备把两者塞进一个通道。兜底靠 `is_rtcp()`：RTP 第 2 字节是
> `M(1)+PT(7)`（≤127），RTCP 的 PT 是 200~204 独占 8 位，一比就知道。

### 5.3 与 rtpmech 的边界（重要）

**interleaved 模式下，rtpmech 的 `RtpSession` 用不上。**

`RtpSession` 绑的是 `UdpEndpoint`，而 interleaved 的 RTP 从 RTSP 的 TCP 连接里出来。
两套模型对不上，且强行统一会让 rtpmech 背上"TCP 传输层"的包袱。

所以 rtspmech **不依赖 rtpmech**：

```
interleaved 收到的裸字节
  → 应用自己调 rtpmech::RtpPacket::parse()
  → 得到 RtpPacket（纯解析，不涉及网络）
  → 应用自己做源跟踪、RTCP、统计
```

这意味着 TCP 模式下**没有** `RtpSessionStats`、**没有**自动 RTCP、**没有** RTT。
这是设计取舍，不是缺陷——文档里要写清楚，否则用的人会困惑。

---

## 6. 状态机

```
  attach(conn) ──► 活动 ──close() / 对端关闭──► 已关闭
```

只有两个稳定状态。关闭后：

| 接口 | 行为 |
|---|---|
| `send_request` / `send_response` / `send_interleaved` | 返回 `false` |
| `close` | 幂等 |
| `is_open` | `false` |
| `id` | 返回原 Conn 的 id（仍可查） |

---

## 7. CSeq 的处理

**库自动递增**，理由：CSeq 必须严格递增且不重复，应用维护计数器容易漏步/重复；
一旦重复，服务端直接回 400。

```cpp
// 不用管 CSeq
sess->send_request(RequestBuilder("DESCRIBE", url).header("Accept", "application/sdp"));

// 需要覆盖时显式设
sess->send_request(RequestBuilder("OPTIONS", url).cseq(100));
```

**响应不做处理**——响应里的 CSeq 必须原样回填请求里的值，由应用从
`RtspRequest::cseq()` 取。

---

## 8. 实施步骤

| 步 | 内容 | 结束标志 |
|---|---|---|
| 1 | 引入 picohttpparser（vendor 到 `thirdparty/`）；搭 CMakeLists | 空库能编译 |
| 2 | 分帧器（文本 + `$` 帧状态机），纯字节进、消息出 | 单元测试：乱序切片、粘包、边界 |
| 3 | `RtspRequest` / `RtspResponse` 解析（picohttpparser 适配） | 头部查询、CSeq、Session |
| 4 | `RtspSession::attach` 接 netmech Conn，弱回调 | 端到端：假服务端对话 |
| 5 | `parse_transport` + `is_rtcp` | 各种 Transport 写法全覆盖 |
| 6 | 示例：最小 RTSP 客户端（DESCRIBE → SETUP → PLAY） | 能拉通本地假服务器 |

每步都要能编译、测试要能跑——跟前两个库一样。

---

## 9. 风险

| 风险 | 影响 | 处置 |
|---|---|---|
| 分帧状态机的边界情况漏掉 | 消息错位、解析失败 | 用"恶意切片"测试：把同一段流按 1 字节、2 字节、随机长度反复切 |
| `Content-Length` 与实际 body 不符 | 卡住或越界 | 上限 2 MB + 超时（`attach` 时可配） |
| 设备发不规范头部 | 解析失败 | picohttpparser 容错较好；必要时做兼容表 |
| 借用视图被跨回调持有 | 读到脏数据 | 文档强调；测试加负例 |
| 单值 interleaved 的兼容性 | 判错通道 | 先用标准语义；遇到实际设备再按 `is_rtcp()` 兜底 |
