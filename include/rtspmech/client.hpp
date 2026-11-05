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

    /// 连接断开（不带原因）。保留是为了兼容已有实现。
    ///
    /// @note 默认的 `on_closed(client, ec)` 会转调这里 —— 只重写这一个也能
    ///       收到通知，只是拿不到原因。想知道"对端正常走了"还是"被重置"，
    ///       请重写带 `ec` 的那个。
    virtual void on_closed(RtspClient&) {}

    /// 连接断开（带原因）。
    ///
    /// @param[in] ec 空 = 正常关闭（应用主动 teardown / 对端正常 TEARDOWN）；
    ///               非空 = 异常（对端 RST、报文非法、消息超限、保活超时…）。
    ///
    /// @note 默认实现转调 `on_closed(client)`。
    virtual void on_closed(RtspClient& c, std::error_code ec)
    {
        (void)ec;
        on_closed(c);
    }
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

    /// interleaved 模式下用的通道号（推流时发 RTP 用）。
    [[nodiscard]] int channel() const noexcept;

    /// 请求停止（发 TEARDOWN 并断开）。幂等。
    ///
    /// @note **拉流与推流都适用**：不要求已经 PLAY，只要会话还活着就发。
    ///       以前这里判断的是"是否正在播放"，于是推流（RECORD）成功后调用
    ///       它会**静默返回**，什么都不做。
    /// @note 不需要告别而想直接断开，用 `session()->close()`。
    void teardown();

    /// 收到 401 后，用 Basic 凭据重发**当前**那个请求。
    ///
    /// @param[in] username 用户名
    /// @param[in] password 密码
    /// @return true  已重发（响应会照常回到回调）
    /// @return false 没有可重发的请求（未连接 / 已完成 / 已失败）
    ///
    /// @par 为什么要这个接口
    ///  401 是"应用决定怎么办"的场景：要不要问用户、要不要换账号，库替不了。
    ///  但以前应用**拿到了决定权却没有执行的能力** —— 状态机推进变量全在
    ///  Impl 里，公共接口只有裸的 `session()`，自己拼一个带 Authorization 的
    ///  请求回去，响应会被映射回已经卡住的 step 上，越弄越乱。
    ///  于是 401 的实际结局几乎总是"干等到对端超时"。
    ///
    /// @note 服务端要的是 Digest 等非 Basic 方案时，用
    ///       `retry_with_authorization()` 自己算好头值。
    [[nodiscard]] bool retry_with_credentials(std::string_view username,
                                              std::string_view password);

    /// 收到 401 后，用**原始的 Authorization 头值**重发当前请求。
    ///
    /// @param[in] header_value 完整的头值，例如 `Digest username="u", ...`
    [[nodiscard]] bool retry_with_authorization(std::string_view header_value);

    /// 放弃当前请求：转入失败终态并回调 `on_failed(stage, unauthorized, 401)`。
    ///
    /// @param[in] stage 阶段名（`"OPTIONS"` / `"DESCRIBE"` / …），仅用于日志
    ///
    /// @note 401 之后如果不想重试，**一定要调它**（或 teardown），否则客户端
    ///       会停在中途：`is_playing()` 是 false、`on_closed` 也不会来。
    void abort_unauthorized(std::string_view stage);

private:
    RtspClient();
    class Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace rtspmech
