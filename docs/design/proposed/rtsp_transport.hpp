// =============================================================================
//  rtspmech - Transport 头解析（讨论稿 v0.1）
// =============================================================================
//  RTSP 的 Transport 头决定"RTP 走哪里"——是 UDP 独立端口，还是复用 RTSP 的
//  TCP 连接（interleaved）。这是 SETUP / SETUP 响应里最关键的一个头。
//
//  字段设计参考了 live555 / ffmpeg 的 rtsp_header_transport_t：
//  那套字段已经把各种设备发来的组合覆盖全了，不必另起一套。
// =============================================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace rtspmech {

/// RTP 的承载方式。
enum class TransportMode {
    /// `RTP/AVP` / `RTP/AVP/UDP`：RTP 走独立的 UDP 端口。
    udp,

    /// `RTP/AVP/TCP`：RTP 复用 RTSP 的 TCP 连接，用 `$` 帧封装。
    interleaved,
};

/// 解析后的 Transport 头。
///
/// 对应这些写法（都是实际设备会发的）：
/// ```
/// RTP/AVP;unicast;client_port=4588-4589;server_port=6256-6257
/// RTP/AVP;multicast;destination=224.2.0.1;port=3456-3457;ttl=16
/// RTP/AVP/TCP;unicast;interleaved=0-1
/// RTP/AVP/TCP;unicast;interleaved=0          ← 很多国产 IPC 只给一个
/// RTP/AVP;unicast;client_port=5000-5001;ssrc=1234ABCD
/// ```
struct RtspTransport {
    TransportMode mode      = TransportMode::udp;
    bool          multicast = false;

    /// 组播目标 / 源（`destination=` / `source=`），仅组播有意义。
    std::string_view destination;
    std::string_view source;

    /// 单播 UDP：客户端端口对与 SSRC（SETUP 请求由客户端填，响应由服务端回填）。
    std::uint16_t client_rtp_port  = 0;
    std::uint16_t client_rtcp_port = 0;
    std::uint16_t server_rtp_port  = 0;
    std::uint16_t server_rtcp_port = 0;
    std::uint32_t ssrc             = 0;

    /// 组播：端口对与 TTL。
    std::uint16_t mcast_rtp_port  = 0;
    std::uint16_t mcast_rtcp_port = 0;
    std::uint8_t  ttl             = 0;

    /// interleaved：两个通道号。
    ///
    /// 按 RFC 2326 §12.39 的标准语义：
    /// - `interleaved=0-1` → `ch1 = 0`（RTP）、`ch2 = 1`（RTCP）；
    /// - `interleaved=0`（只给一个）→ **隐含 `ch2 = ch1 + 1`**，
    ///   即 RTP 用偶数通道、RTCP 用紧随其后的奇数通道。
    ///
    /// @note 确实有设备把两者塞进同一个通道；那种情况靠 `is_rtcp()` 判载荷类型
    ///       兜底，不要因为这个把解析改掉。
    int ch1 = -1;   ///< RTP 通道
    int ch2 = -1;   ///< RTCP 通道（单值写法时 = ch1 + 1）
};

/// 解析一个 `Transport:` 头的值（不含头名）。
///
/// @param[in]  value `"RTP/AVP;unicast;client_port=4588-4589;server_port=6256-6257"`
/// @param[out] out   解析结果
/// @return true 表示解析成功且是已知的承载方式
///
/// @note 同一个头里可能有**多段**（逗号分隔），表示多个候选传输方式。
///       调用方应先按 `,` 切开、逐段调用本函数，取第一个能接受的
///       （客户端通常在 SETUP 里给 2~3 段）。
[[nodiscard]] bool parse_transport(std::string_view value, RtspTransport& out) noexcept;

} // namespace rtspmech
