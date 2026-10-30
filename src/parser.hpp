// =============================================================================
//  parser.hpp —— RTSP 消息解析（内部头）
// =============================================================================
//  用 picohttpparser 解析"请求行/状态行 + 头部"，再补上 RTSP 独有的语义。
//
//  为什么 picohttpparser 能解析 RTSP：
//    RTSP 的请求行 `OPTIONS rtsp://... RTSP/1.0` 与 HTTP 的三段式结构相同，
//    状态行 `RTSP/1.0 200 OK` 也一样。picohttpparser 只切分、不校验方法名
//    与版本字符串，所以两协议通用。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

#include <rtspmech/session.hpp>

namespace rtspmech {
namespace detail {

/// 头部字段数量上限。RTSP 头部没那么多，64 足够；超出的部分丢弃。
constexpr std::size_t kMaxHeaders = 64;

/// 解析结果（借用视图，指向传入的 data）。
struct ParsedMessage {
    enum class Kind { request, response } kind = Kind::request;

    // ---- 请求行 ----
    std::string_view method;    ///< "OPTIONS" / "DESCRIBE" / ...
    std::string_view uri;       ///< "rtsp://..."

    // ---- 状态行 ----
    int              status = 0;
    std::string_view reason;    ///< "OK" / "Unauthorized" / ...

    std::string_view version;   ///< "RTSP/1.0"

    // ---- 共同 ----
    RtspHeader  headers[kMaxHeaders];
    std::size_t num_headers = 0;

    const std::uint8_t* body     = nullptr;
    std::size_t         body_len = 0;
};

/// 解析一条完整的 RTSP 文本消息（已由 Framer 切好）。
///
/// @param[in] data 可写缓冲。picohttpparser 硬编码校验 "HTTP/"，
///                 这里会把开头的 "RTSP" 原地换成 "HTTP"（两者都是 4 字节，
///                 长度不变）再交给它。改完不回滚——上层本来就把 version
///                 固定成 "RTSP/1.0"。
/// @return true 表示成功；false 时 ec 说明原因
///
/// @note 自动判断是请求还是响应：首 token 以 "RTSP/" 开头即响应。
[[nodiscard]] bool parse_message(std::uint8_t* data, std::size_t len,
                                 ParsedMessage& out, std::error_code& ec) noexcept;

/// ASCII 大小写不敏感比较。
[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept;

/// 在头部列表里按名字查找（大小写不敏感）。
[[nodiscard]] std::string_view find_header(const RtspHeader* headers, std::size_t n,
                                           std::string_view name) noexcept;

/// 解析十进制无符号整数（用于 CSeq）。
[[nodiscard]] std::uint32_t parse_u32(std::string_view s) noexcept;

} // namespace detail
} // namespace rtspmech
