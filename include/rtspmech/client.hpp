// =============================================================================
//  rtspmech - 完整的 RTSP 客户端
// =============================================================================
//  文件    : rtspmech/client.hpp
//  用途    : 把 OPTIONS → DESCRIBE → SETUP → PLAY 这套流程封装成一个对象。
//
//  ★ 为什么放在库里而不是示例里
//  这套流程是**每个 RTSP 应用都要写的**（拉流、推流、探测设备能力）。放在
//  示例里意味着每个人抄一遍；放在库里则只需要配一个回调。
//
//  ★ 它做什么
//    * 状态机：OPTIONS → DESCRIBE → SETUP（每个 track 一次）→ PLAY
//    * CSeq 由 `RtspSession` 自动分配
//    * 会话 ID 从 SETUP 的响应里取，后续请求自动带上
//    * 解析 SDP 里的 `a=control:` 得到 track 列表
//    * 按客户端要求的 transport 协商（UDP 或 interleaved）
//
//  ★ 它不做什么
//    * 不解析 SDP 的媒体描述（只取 control）
//    * 不做认证（收到 401 就回调应用，由应用决定）
//    * 不管理媒体收发（UDP 用 netmech 的 UdpEndpoint，interleaved 用 session）
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <rtspmech/session.hpp>

namespace rtspmech {

class RtspClient;   // 前置声明（ClientHandler 里用到）

/// 客户端的工作模式。
enum class ClientMode {
    /// 拉流：DESCRIBE → SETUP → PLAY。从服务端取流。
    pull,
    /// 推流：ANNOUNCE → SETUP → RECORD。往服务端送流。
    push,
};

/// 客户端的传输方式偏好。
enum class TransportPreference {
    /// `RTP/AVP;unicast;client_port=...` —— 媒体走独立 UDP 端口。
    udp,
    /// `RTP/AVP/TCP;unicast;interleaved=0-1` —— 媒体复用在 RTSP 连接里。
    interleaved,
};

/// 客户端事件。
class ClientHandler {
public:
    virtual ~ClientHandler() = default;

    /// 拉流完成（PLAY 成功）。
    virtual void on_playing(RtspClient&) {}

    /// 推流完成（RECORD 成功）—— 可以开始发 RTP 了。
    virtual void on_recording(RtspClient&) {}

    /// 流程失败。
    ///
    /// @param[in] stage  失败发生在哪一步（"OPTIONS"/"DESCRIBE"/"SETUP"/"PLAY"）
    /// @param[in] ec     传输层错误（空表示传输成功，失败原因看 status）
    /// @param[in] status 服务端返回的状态码（非 2xx 即失败）
    virtual void on_failed(RtspClient&, const char* stage, std::error_code ec,
                           int status) = 0;

    /// 服务端要求认证。
    virtual void on_unauthorized(RtspClient&, std::string_view /*www_authenticate*/) {}

    /// 连接断开。
    virtual void on_closed(RtspClient&) {}
};

/// RTSP 客户端。
class RtspClient : public std::enable_shared_from_this<RtspClient> {
public:
    struct Config {
        /// 工作模式。
        ClientMode mode = ClientMode::pull;

        /// 流地址（`rtsp://host:port/path`）。
        ///
        /// @note 推流模式下，**每路必须用不同的 URL**——服务端靠它区分不同
        ///       的源，分发时也靠它定位。
        std::string url;

        /// 推流模式的 SDP（`mode == push` 时必填）。
        ///
        /// @note 这是**客户端告诉服务端"我要推什么"**，不是服务端返回的。
        ///       内容至少要有 `m=video 0 RTP/AVP 96` + `a=rtpmap:96 H264/90000`。
        std::string sdp;

        /// 传输偏好。
        TransportPreference transport = TransportPreference::udp;

        /// UDP 模式下客户端要绑定的端口对（RTP / RTCP）。
        /// 0 表示由系统分配。
        std::uint16_t client_rtp_port = 0;

        /// interleaved 模式下的通道号（默认 0-1）。
        int channel = 0;

        /// 是否在 PLAY 之后自动保持（不发 TEARDOWN）。
        bool keep_alive = true;
    };

    /// 创建并发起连接。
    [[nodiscard]] static std::shared_ptr<RtspClient>
    create(const Config& cfg, std::shared_ptr<ClientHandler> handler,
           std::error_code& ec);

    ~RtspClient();

    // ---- 查询 ----

    [[nodiscard]] bool         is_playing() const noexcept;
    /// 推流模式下 RECORD 是否成功。
    [[nodiscard]] bool         is_recording() const noexcept;
    [[nodiscard]] ClientMode   mode() const noexcept;
    [[nodiscard]] const std::string& session_id() const noexcept;
    [[nodiscard]] const std::string& sdp() const noexcept;
    [[nodiscard]] std::uint16_t server_rtp_port() const noexcept;

    /// 拿到底层会话（发 TEARDOWN、发媒体用）。
    [[nodiscard]] RtspSession* session() noexcept;

    /// 推流模式下：服务端告诉我们的**收流端口**（UDP）。
    ///
    /// @note 应用要往 `url 里的 host : server_udp_port()` 发 RTP。
    [[nodiscard]] std::uint16_t server_udp_port() const noexcept;

    /// 推流模式下：服务端告诉我们的 RTCP 端口（UDP）。
    [[nodiscard]] std::uint16_t server_rtcp_port() const noexcept;

    /// 请求停止（发 TEARDOWN 并断开）。
    void teardown();

private:
    RtspClient();
    class Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace rtspmech
