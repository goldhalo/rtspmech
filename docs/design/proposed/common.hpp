// =============================================================================
//  rtspmech - 基础类型与错误码（讨论稿）
// =============================================================================
#pragma once

#include <string>
#include <system_error>

namespace rtspmech {

/// rtspmech 错误码。
enum class RtspErrc {
    ok = 0,
    invalid_message,      ///< 报文格式错（请求行/状态行/头部）
    unsupported_version,  ///< 不是 RTSP/1.0
    too_large,            ///< 单条消息超过上限（默认 2 MB，防 Content-Length 被打爆）
    not_attached,         ///< 会话已关闭
    send_failed,          ///< 底层发送失败
    invalid_transport,    ///< Transport 头无法解析
};

[[nodiscard]] const std::error_category& rtsp_category() noexcept;
[[nodiscard]] std::error_code make_error_code(RtspErrc e) noexcept;

} // namespace rtspmech

namespace std {
template <>
struct is_error_code_enum<rtspmech::RtspErrc> : true_type {};
} // namespace std
