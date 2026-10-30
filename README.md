# rtspmech

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)](#平台支持)

> 基于 [netmech](../netmech) 的 **RTSP 协议胶水层**（C++17）。
> 网络用 netmech，解析用 picohttpparser，剩下的就是那一套弱回调。

---

## 定位

| 做 | 不做 |
|---|---|
| 从 TCP 字节流切出 RTSP 消息（文本 + `$` 帧混流） | 推流、取流（那是应用） |
| 用 picohttpparser 解析请求行 / 状态行 / 头部 | SDP 深度解析（只给原始 body） |
| RTSP 语义辅助：`CSeq` / `Session` / `Transport` / `$` 帧 | Digest / Basic 认证握手 |
| 弱回调把消息抛给应用 | 会话状态机、重连、保活策略 |

一句话：**给一块 TCP 连接，它负责把方言切成句子，剩下全靠应用。**

---

## 30 秒上手

```cpp
#include <rtspmech/rtspmech.hpp>
#include <netmech/net.hpp>

using namespace rtspmech;

// 1) 实现 Linkable：每个新连接包一个 RtspSession
class MyServer : public netmech::Linkable, public RtspHandler,
                 public std::enable_shared_from_this<MyServer> {
public:
    std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn& c) override {
        std::error_code ec;
        auto self = shared_from_this();
        return RtspSession::create(c.shared_from_this(), self, ec);
    }

    void on_request(RtspSession& sess, const RtspRequest& req) override {
        auto wire = ResponseBuilder(200, "OK").cseq(req.cseq()).build();
        sess.send_response(std::move(wire));
    }
};

// 2) 交给 netmech 驱动
auto mech = netmech::Mechanism::create();
auto srv  = std::make_shared<MyServer>();
mech->set_linkable(srv);
mech->listen(netmech::Address{"0.0.0.0", 554}, srv);
```

客户端侧多一个 `on_ready`（连接就绪后在那里发第一个请求）：

```cpp
void on_ready(RtspSession& s) override {
    RequestBuilder b("OPTIONS", url);
    s.send_request(b);          // CSeq 自动分配
}
```

完整可跑的例子见 `example/rtsp_demo.cpp`（进程内假服务端 + 客户端，走完 DESCRIBE → SETUP → PLAY）。

---

## 三个必须知道的约束

### ① `RtspSession` 是 `ConnHandler`，不是"接管器"

netmech 的连接处理器**创建连接时绑定**，之后不能换。所以 RTSP 会话不是"attach 一个已有连接"，
而是在 `on_accept` / `on_linkup` 里创建、作为返回值交给 netmech。

### ② 第一个请求要在 `on_ready` 里发

netmech 的时序是 `on_linkup` 返回 handler → 库调 `on_open()` → 之后才能读写。
在 `on_linkup` 里刚 create 出来就 `send_request()` 是**发不出去的**（连接还没被驱动）。

### ③ 回调里的字段是借用视图

`RtspRequest` / `RtspResponse` 的 `method` / `uri` / `headers` / `body` 都指向接收缓冲，
**回调返回即失效**。要留就自己拷：

```cpp
m_sdp.assign(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
```

---

## interleaved（`$` 帧）

RTP/RTCP 复用到 RTSP 的 TCP 连接时，用 4 字节帧头封装：

```
$ | channel(1) | length(2, 大端) | payload
```

收到的帧走 `on_interleaved(channel, data, len)`。要区分 RTP 和 RTCP：

```cpp
if (is_rtcp(data, len)) { /* RTCP */ } else { /* RTP */ }
```

规则：RTP 第 2 字节是 `M(1)+PT(7)`（PT ≤ 127），RTCP 的 PT 是 200~204 独占 8 位。
**分两个 channel 和复用一个 channel 的设备都适用。**

> ⚠️ **interleaved 模式下 rtpmech 的 `RtpSession` 用不上** —— 它绑的是 UDP。
> 应用拿到裸字节后自己调 `rtpmech::RtpPacket::parse()`，但没有自动 RTCP / RTT / stats。

---

## Transport 头

```cpp
RtspTransport t;
parse_transport("RTP/AVP/TCP;unicast;interleaved=0-1", t);
// t.mode == TransportMode::interleaved, t.ch1 == 0, t.ch2 == 1
```

覆盖的写法：

| 写法 | 解析结果 |
|---|---|
| `RTP/AVP;unicast;client_port=4588-4589;server_port=6256-6257` | UDP 单播，端口对 |
| `RTP/AVP;multicast;destination=...;port=...;ttl=16` | UDP 组播 |
| `RTP/AVP/TCP;interleaved=0-1` | interleaved 两个通道 |
| `RTP/AVP/TCP;interleaved=0` | **ch2 = ch1 + 1**（RFC 2326 §12.39） |
| `...;ssrc=1234ABCD` | 十六进制 SSRC |

---

## 构建

### 依赖

- C++17 编译器
- CMake ≥ 3.16
- **netmech**（git submodule）
- **picohttpparser**（vendored 到 `thirdparty/`）

### Linux / WSL

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
      -DRTSPMECH_NETMECH_DIR=/path/to/netmech
cmake --build build-linux -j$(nproc)
ctest --test-dir build-linux --output-on-failure
```

### aarch64 交叉编译

```bash
cmake -S . -B build-aarch64 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
      -DRTSPMECH_NETMECH_DIR=/path/to/netmech
cmake --build build-aarch64 -j
```

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `RTSPMECH_BUILD_TESTS` | ON | 单元测试（29 用例 / 139 断言） |
| `RTSPMECH_BUILD_EXAMPLES` | ON | `rtsp_demo` |
| `RTSPMECH_WARNINGS_AS_ERRORS` | OFF | CI 用 |

---

## 目录结构

```
include/rtspmech/   公共头（5 个）
  span.hpp          轻量 span
  common.hpp        RtspErrc
  rtsp_transport.hpp Transport 解析 + is_rtcp
  session.hpp       RtspSession / RtspHandler / Builder
  rtspmech.hpp      汇总头

src/                实现（含内部头 framer / parser）
test/               29 用例 / 139 断言
example/            rtsp_demo
thirdparty/         picohttpparser（vendored）
docs/design/        概要设计
```

---

## 为什么 picohttpparser 能解析 RTSP

它其实**不能**——`parse_http_version` 硬编码校验 `HTTP/`（`picohttpparser.c:295-299`）。

好在 `RTSP` 和 `HTTP` 都是 4 个字母，解析前把第一行里的 `RTSP` 原地换成 `HTTP`
即可（长度不变、零拷贝）。请求的 `RTSP` 在行尾、响应的在行首，所以是在第一行里搜。

---

## 相关项目

- [netmech](../netmech) —— 底层网络库
- [rtpmech](../rtpmech) —— RTP/RTCP 协议库

---

## 致谢

本项目的实现与文档在 **DeepSeek-V4.1** 的协助下完成 —— 架构决策与验收由作者负责，
详见 [AUTHORS.md](AUTHORS.md)。

---

## License

MIT，见 [LICENSE](LICENSE)。
