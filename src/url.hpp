// =============================================================================
//  url.hpp —— 客户端 URL 解析（内部头，独立出来便于单测）
// =============================================================================
#pragma once

#include "rtspmech/export.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace rtspmech {
namespace detail {

struct ParsedUrl {
    std::string   host;
    std::uint16_t port = 554;
    std::string   path;
    bool          ok = false;
};

/// 拆 `rtsp://host[:port]/path`。
///
/// 支持 **IPv6 字面量**：`rtsp://[::1]:554/x` / `rtsp://[2001:db8::1]/y`
/// （host 返回去括号的 `::1`、`2001:db8::1`，可直接交给 netmech::Address）。
/// 不支持 userinfo（`rtsp://user:pass@host/`）—— 会解析失败而不是猜。
[[nodiscard]] RTSPMECH_API ParsedUrl parse_url(std::string_view text) noexcept;

} // namespace detail
} // namespace rtspmech
