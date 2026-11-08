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

using namespace rtspmech;

// 1) 实现 RtspHandler：收到请求就回 200
class MyHandler : public RtspHandler {
public:
    void on_request(RtspSession& sess, const RtspRequest& req) override {
        auto wire = ResponseBuilder(200, "OK").cseq(req.cseq()).build();
        sess.send_response(std::move(wire));
    }
};

// 2) 实现 RtspAcceptor：每条新连接给一个 handler（返回 nullptr = 拒绝）
class MyAcceptor : public RtspAcceptor {
public:
    std::shared_ptr<RtspHandler> on_accept(const Endpoint& /*peer*/) override {
        return std::make_shared<MyHandler>();
    }
};

// 3) 起服务：io 运行时由库自己建，不需要认识 netmech
RtspServerConfig cfg;
cfg.local = {"0.0.0.0", 554};

auto srv = RtspServer::create(cfg, std::make_shared<MyAcceptor>());
if (auto ec = srv->start()) { /* 端口占用等 */ }

// 端口 0 时这样拿系统分配的端口：
//   Endpoint bound = srv->local();
```

想连出去当客户端：`RtspConnector::create({.remote = {"10.0.0.5", 554}}, handler)`，
连上后 handler 会在 `on_ready(sess)` 里拿到会话。

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

### ① 会话由库创建，应用不自己造

`RtspSession` 是个普通类（不继承任何外部接口）。它由 `RtspServer` /
`RtspConnector` 在连接就绪时创建，再通过 `RtspHandler::on_ready(sess)` 交给应用。
**没有 `RtspSession::create()` 这种入口** —— 你也拿不到一个"裸连接"去包。

### ② 第一个请求要在 `on_ready` 里发

会话创建完、底层连接被驱动起来之后，库才回调 `on_ready`。
在这之前 `send_request()` 是**发不出去的**（连接还没开始读写）。
客户端的"连上就发 OPTIONS"就写在这里：

```cpp
void on_ready(RtspSession& s) override {
    RequestBuilder b("OPTIONS", m_url);
    s.send_request(b);
}
```

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
- **netmech**（git submodule，**链接期依赖**；头文件不需要 —— 见下）

> **公共头不依赖 netmech**：`include/rtspmech/*.hpp` 里没有 `#include <netmech/...>`，
> 地址用 `rtspmech::Endpoint`。所以编译本库的代码不必把 netmech 的头挂进 include 路径。
> 但 `librtspmech.a` 内部调用 netmech，**链接时仍然要 `libnetmech.a`**
> （`find_package(rtspmech)` 会自动带出来）。
>
> picohttpparser 是 vendored 的，已经**编进 librtspmech.a**，不再单独出库、也不装它的头。

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

### 示例对 rtpmech 的依赖

`example/rtsp_server.cpp`（推流示例）用 rtpmech 的 H.264 打包器，默认从
`../rtpmech` 取；不在那个位置就显式给：

```bash
cmake -S . -B build -DRTSPMECH_RTPMECH_DIR=/path/to/rtpmech
```

找不到时会**跳过**这个示例（不会让构建失败），其余目标照常编译。

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `RTSPMECH_BUILD_TESTS` | ON | 单元测试 |
| `RTSPMECH_BUILD_EXAMPLES` | ON | `rtsp_demo` |
| `RTSPMECH_WARNINGS_AS_ERRORS` | OFF | CI 用 |

---

## 目录结构

```
include/rtspmech/   公共头
  span.hpp          轻量 span
  endpoint.hpp      端点（IP + 端口，不引 netmech）
  common.hpp        RtspErrc
  rtsp_transport.hpp Transport 解析 + is_rtcp
  session.hpp       RtspSession / RtspHandler / Builder
  client.hpp        RtspClient（把常见流程封装成一步到位）
  server.hpp        RtspServer / RtspAcceptor
  connector.hpp     RtspConnector（客户端）
  rtspmech.hpp      汇总头

src/                实现（含内部头 framer / parser）
test/               单元测试
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

## 在其他项目里使用（find_package）

先装 netmech，再装 rtspmech：

```bash
cmake -S ../netmech -B ../netmech/build && cmake --build ../netmech/build -j
cmake --install ../netmech/build

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build                  # 默认前缀：Linux /usr/local
```

然后：

```cmake
find_package(rtspmech REQUIRED)
target_link_libraries(app PRIVATE rtspmech::rtspmech)
```

`librtspmech.a + libnetmech.a + pthread` 这几个**不用手写** —— 导入的 target 全带着。

装出来的结构：

```
<prefix>/include/rtspmech/*.hpp                     # 只有本库的头
<prefix>/lib/librtspmech.a                          # picohttpparser 已编进来
<prefix>/lib/cmake/rtspmech/rtspmech{Config,ConfigVersion,Targets}.cmake
<prefix>/share/doc/rtspmech/LICENSE.picohttpparser  # MIT 要求保留的声明
```

想控制 io 线程数（默认 = CPU 核数）或与同进程其它库共用一份线程池：
`#include <netmech/common.hpp>` 后调一次 `netmech::initialize(n)`，或者用
`RtspServer::create_with_mechanism() / RtspConnector::create_with_mechanism()`。

---

## 致谢

本项目的实现与文档在 **DeepSeek-V4.1** 的协助下完成 —— 架构决策与验收由作者负责，
详见 [AUTHORS.md](AUTHORS.md)。

---

## License

MIT，见 [LICENSE](LICENSE)。

## 动态库与 Runtime 注入

```bash
cmake -S . -B build-shared -DRTSPMECH_LIBRARY_TYPE=SHARED
```

- 公共 API 用 `RTSPMECH_API` 导出（Windows `dllexport/dllimport`，Linux hidden 可见性
  + `-Wl,--exclude-libs,ALL` 隐藏静态链入的 netmech / picohttpparser 符号）；
- SHARED 构建会把 netmech 也编成共享库（`libnetmech.so` / `netmech.dll`）：
  两端必须在同一份 io 运行时上，否则 Runtime 注入会跨副本使用（netmech 明确禁止，
  rtspmech 的测试里实测会段错误）。所以发布时 **rtspmech 与 netmech 两个共享库一起发**，
  并保留 netmech 的 MIT 许可证声明。

```cpp
#include <netmech/runtime.hpp>

netmech::Runtime::Options o;
o.io_threads = 2;                       // 应用自己决定
auto rt = netmech::Runtime::create(o);

rtspmech::RtspServerConfig cfg;
cfg.local   = {"0.0.0.0", 554};
cfg.runtime = rt;                       // server / connector / client 都支持

auto srv = rtspmech::RtspServer::create(cfg, acceptor);
```

不传 `runtime` 时行为与以前一致（进程级默认运行时）。同进程多模块请让 netmech 以
共享库形式被共用（见 netmech `docs/packaging.md`）。
