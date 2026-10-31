// =============================================================================
//  rtspmech - 会话对外接口（讨论稿 v0.1）
// =============================================================================
//  本文件是**接口草案**，用于讨论，不是可用代码。
//
//  定位：RTSP 的**协议胶水层**。
//    · 网络：netmech 的 Conn（TCP）
//    · 解析：picohttpparser（HTTP 风格的请求行 / 状态行 / 头部）
//    · 剩下：RTSP 独有的语义（CSeq / Session / Transport / $ 帧）+ 弱回调
//    · **不做**：推流、取流、SDP 深度解析、会话管理策略——那些是应用的事
//
//  与 rtpmech 的关系：**不依赖**。
//    interleaved 模式下 RTP 从 RTSP 的 TCP 连接里出来，rtpmech 的 RtpSession
//    绑的是 UdpEndpoint，用不上；应用拿到裸字节后自己调 rtpmech::RtpPacket::parse。
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>

#include <netmech/conn.hpp>

#include <rtspmech/span.hpp>

namespace rtspmech {

class RtspSession;

// =============================================================================
//  消息视图（借用，零拷贝）
// =============================================================================

/// 一个头部字段。`name` / `value` 都指向**接收缓冲**，回调返回即失效。
struct RtspHeader {
    std::string_view name;
    std::string_view value;
};

/// 收到的 RTSP 请求（服务端用）。
///
/// @warning 所有 `string_view` / `span` 都是**借用视图**，指向解包用的接收缓冲。
///          回调返回后失效。要留就自己拷。
struct RtspRequest {
    std::string_view      method;    ///< "OPTIONS" / "DESCRIBE" / "SETUP" / "PLAY" / ...
    std::string_view      uri;       ///< "rtsp://192.168.1.10:554/stream"
    std::string_view      version;   ///< "RTSP/1.0"
    span<const RtspHeader> headers;
    span<const std::uint8_t> body;   ///< 消息体（DESCRIBE 的响应带 SDP；请求一般没有）

    /// 按名字查头部（大小写不敏感）。找不到返回空 view。
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;

    /// `CSeq`（请求序号）。缺失或非法返回 0。
    [[nodiscard]] std::uint32_t cseq() const noexcept;

    /// `Session`（会话 ID，不含 `;timeout=` 后缀）。缺失返回空。
    [[nodiscard]] std::string_view session() const noexcept;
};

/// 收到的 RTSP 响应（客户端用）。
struct RtspResponse {
    std::string_view      version;   ///< "RTSP/1.0"
    int                   status = 0;///< 200 / 401 / 404 / 454 / ...
    std::string_view      reason;    ///< "OK" / "Unauthorized" / ...
    span<const RtspHeader> headers;
    span<const std::uint8_t> body;   ///< DESCRIBE 的响应体是 SDP

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    [[nodiscard]] std::uint32_t cseq() const noexcept;
    [[nodiscard]] std::string_view session() const noexcept;

    /// 401 时用来判断该用哪种认证（`WWW-Authenticate: Digest ...` / `Basic ...`）。
    [[nodiscard]] std::string_view www_authenticate() const noexcept;
};

// =============================================================================
//  发出去的消息（由调用方构造）
// =============================================================================

/// 构造一个请求。用链式调用填字段，最后一次 `build()` 成字节流。
///
/// 之所以不直接给 `RtspRequest`（借用视图）来发送，是因为发送时数据必须由
/// 调用方持有到写出完成；用 builder 内部持有所有权，语义更清楚。
class RequestBuilder {
public:
    RequestBuilder(std::string_view method, std::string_view uri);

    RequestBuilder& header(std::string_view name, std::string_view value);

    /// 显式指定 CSeq。**一般不需要调用** —— `RtspSession::send_request()`
    /// 会自动分配一个递增的序号。
    RequestBuilder& cseq(std::uint32_t seq);

    /// 消息体（SDP 等）。内部拷贝一份。
    RequestBuilder& body(std::string_view data);

    /// 生成完整报文（含 `\r\n\r\n` 与 body）。
    [[nodiscard]] std::string build() const;

    /// 由库在发送前调用：调用方没显式设 CSeq 时填入给定值。
    void assign_cseq_if_absent(std::uint32_t seq);

    [[nodiscard]] bool has_cseq() const noexcept { return m_has_cseq; }

private:
    std::string m_head;
    std::string m_body;
    bool        m_has_cseq = false;
};

/// 构造一个响应（服务端用）。
class ResponseBuilder {
public:
    ResponseBuilder(int status, std::string_view reason);

    ResponseBuilder& header(std::string_view name, std::string_view value);
    ResponseBuilder& cseq(std::uint32_t seq);
    ResponseBuilder& body(std::string_view data);

    [[nodiscard]] std::string build() const;

private:
    std::string m_head;
    std::string m_body;
};

// =============================================================================
//  事件
// =============================================================================

/// RTSP 会话上的事件。全部在该 Conn 的 io 线程上回调，**同一会话严格串行**。
class RtspHandler {
public:
    virtual ~RtspHandler() = default;

    /// 会话就绪：可以开始发请求了。**每个会话恰好一次**。
    ///
    /// @par 为什么需要这个
    ///  netmech 的时序是：`Linkable::on_linkup/on_accept` 返回 handler →
    ///  库再调用 `handler->on_open()` → 之后才能读写。所以应用在
    ///  `on_linkup` 里刚 `RtspSession::create()` 出来就 `send_request()` 是发不出去的
    ///  （连接还没被驱动）。要发第一个请求，在这里发。
    ///
    ///  @code
    ///      void on_ready(RtspSession& s) override {
    ///          s.send_request(RequestBuilder("OPTIONS", url));
    ///      }
    ///  @endcode
    virtual void on_ready(RtspSession& sess) { (void)sess; }

    /// 收到一个 RTSP 请求。**服务端**实现这个。
    virtual void on_request(RtspSession& sess, const RtspRequest& req) { (void)sess; (void)req; }

    /// 收到一个 RTSP 响应。**客户端**实现这个。
    virtual void on_response(RtspSession& sess, const RtspResponse& resp) { (void)sess; (void)resp; }

    /// 收到一个 interleaved 二进制帧（`$` 帧）。
    ///
    /// @param[in] channel 帧头里的 channel 号
    /// @param[in] data    帧载荷（**不含 4 字节帧头**），借用视图
    /// @param[in] len     载荷长度
    ///
    /// @note RTP 与 RTCP 可能在**同一个 channel** 上（很多设备如此），也可能
    ///       分在 `channel` / `channel+1`（RFC 2326 的标准做法）。两种情况都用
    ///       这个回调，判断方法见 `is_rtcp()`。
    virtual void on_interleaved(RtspSession& sess, int channel,
                                const std::uint8_t* data, std::size_t len)
    {
        (void)sess; (void)channel; (void)data; (void)len;
    }

    /// 连接关闭。恰好一次。
    ///
    /// @param[in] sess 关闭的那个会话。
    ///
    /// @par 为什么带 session 参数
    ///  一个 handler 经常被**多个会话共用**（比如中转服务器：所有客户端
    ///  连同一个 handler）。没有这个参数的话，handler 无法知道"是哪个
    ///  会话关了"，只能在表里累积失效的键 —— 内存涨、订阅者列表里留着
    ///  已断开的会话，转发时往死连接上发。
    virtual void on_close(RtspSession& sess, std::error_code ec)
    {
        (void)sess;
        (void)ec;
    }
};

// =============================================================================
//  会话
// =============================================================================

/// 一个 RTSP 连接。
///
/// @par 为什么是 ConnHandler
///  netmech 的连接处理器是**创建连接时绑定**的，之后不能换。所以 RTSP 会话
///  不是"接管"一个已有连接，而是**本身就是**该连接的 handler：
///
///  @code
///      class MyLinkable : public netmech::Linkable {
///          std::shared_ptr<netmech::ConnHandler>
///          on_accept(std::shared_ptr<netmech::Conn> c) override {
///              return RtspSession::create(c, m_rtsp_handler);
///          }
///      };
///      mech->set_linkable(my_linkable);   // 服务端
///      mech->connect(addr, my_linkable);  // 客户端
///  @endcode
///
/// @par 它做什么
///  - 从 Conn 读字节流，按 RTSP 消息边界切分（`\r\n\r\n` + Content-Length）
///  - `$` 开头的字节走二进制帧路径，其余走文本解析（picohttpparser）
///  - 解析出的消息通过 `RtspHandler` 抛给应用
///
/// @par 它不做什么
///  - 不发请求、不做重试、不维护会话状态
///  - 不解析 SDP、不做认证握手
///  - 这些都是应用的事
///
/// @par 线程
///  与 netmech 一致：同一会话的回调串行，不需要加锁。可从任意线程调用 `send_*`。
class RtspSession : public netmech::ConnHandler,
                    public std::enable_shared_from_this<RtspSession> {
public:
    /// 为一个刚建立的连接创建 RTSP 会话。
    ///
    /// @param[in] conn    由 netmech 的 `on_accept` / `on_linkup` 拿到的连接
    /// @param[in] handler 事件处理器（库以 **weak_ptr** 持有）
    /// @param[out] ec     失败原因
    ///
    /// @note 返回的对象**必须**交回给 netmech（作为 `on_accept` 的返回值），
    ///       由 netmech 负责持有与驱动。自己拿着不返回，这个连接不会被读。
    [[nodiscard]] static std::shared_ptr<RtspSession>
    create(std::shared_ptr<netmech::Conn>        conn,
           std::weak_ptr<RtspHandler>            handler,
           std::error_code&                      ec);

    ~RtspSession();
    RtspSession(const RtspSession&) = delete;
    RtspSession& operator=(const RtspSession&) = delete;

    // ---- 发送 ----

    /// 发一个请求（客户端）。
    ///
    /// **CSeq 由库自动分配**：若 builder 里没显式调用过 `cseq()`，这里会填一个
    /// 会话内递增的序号。这让应用不必自己维护计数器——那个计数器一旦漏步或
    /// 重复，服务端会直接回 400。
    ///
    /// @return true 表示已受理；false 表示连接已关闭或报文为空
    bool send_request(RequestBuilder& req);

    /// 发一个响应（服务端）。不做 CSeq 处理 —— 响应里的 CSeq 必须**原样回填**
    /// 请求里的值，由应用从 `RtspRequest::cseq()` 取。
    bool send_response(std::string wire);

    /// 发一个 interleaved 二进制帧（`$` 帧）。服务端推流时用。
    ///
    /// @param[in] channel 通道号（与 SETUP 响应里告诉客户端的一致）
    bool send_interleaved(int channel, const std::uint8_t* data, std::size_t len);

    // ---- 查询 ----

    [[nodiscard]] bool          is_open() const noexcept;
    [[nodiscard]] std::uint64_t id() const noexcept;   ///< 底层 Conn 的 id

    // ---- 关闭 ----

    /// 关闭会话（关底层 Conn）。幂等。
    void close() noexcept;

    // ---- netmech::ConnHandler ----
    //
    // 这四个由 netmech 驱动，应用不要直接调用。
    //
    // 实现要点：netmech 的读是**拉模式**（自己提供缓冲、完成时回调），
    // 所以 on_open 里发起第一笔读，每次 on_read 处理完再发起下一笔。
    // TCP 的 FIN 只有在下一次 read 时才感知得到，读循环不能断。

    bool on_open(std::shared_ptr<netmech::Conn> conn) override;
    void on_read(std::error_code ec, netmech::Buf buf, std::size_t n) override;
    void on_write(std::error_code ec, netmech::ConstBuf buf, std::size_t n) override;
    void on_close() override;

private:
    RtspSession();
    class Impl;
    std::shared_ptr<Impl> m_impl;
};

// =============================================================================
//  辅助
// =============================================================================

// is_rtcp() 见 rtspmech/rtsp_transport.hpp —— 它跟 interleaved 同属"载荷怎么走"的话题

} // namespace rtspmech
