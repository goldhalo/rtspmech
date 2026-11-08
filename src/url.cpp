// =============================================================================
//  url.cpp —— 客户端 URL 解析实现
// =============================================================================
#include "url.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace rtspmech {
namespace detail {

namespace {

/// 解析十进制端口（1~65535）。失败返回 false。
bool parse_port(std::string_view s, std::uint16_t& out) noexcept
{
    if (s.empty()) {
        return false;
    }
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<unsigned>(c - '0');
        // 每步判上界：只在循环结束后判，`99999999999` 会在 unsigned 上回绕
        if (v > 65535) {
            return false;
        }
    }
    if (v == 0) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

} // namespace

ParsedUrl parse_url(std::string_view s) noexcept
{
    ParsedUrl u;
    constexpr std::string_view prefix = "rtsp://";
    if (s.size() <= prefix.size() || s.substr(0, prefix.size()) != prefix) {
        return u;
    }
    s.remove_prefix(prefix.size());

    const auto slash = s.find('/');
    const std::string_view hostport =
        (slash == std::string_view::npos) ? s : s.substr(0, slash);
    u.path = (slash == std::string_view::npos) ? std::string("/")
                                              : std::string(s.substr(slash));

    // userinfo（user:pass@host）不支持：宁可解析失败，也不猜 host。
    if (hostport.find('@') != std::string_view::npos) {
        return u;
    }

    std::string_view port_str;

    if (!hostport.empty() && hostport.front() == '[') {
        // IPv6 字面量：rtsp://[::1]:554/x
        const auto close = hostport.find(']');
        if (close == std::string_view::npos || close == 1u) {
            return u;   // 缺 ']' 或空地址
        }
        u.host.assign(hostport.substr(1, close - 1));
        if (close + 1 < hostport.size()) {
            if (hostport[close + 1] != ':') {
                return u;
            }
            port_str = hostport.substr(close + 2);
        }
    } else {
        const auto colon = hostport.rfind(':');
        if (colon == std::string_view::npos) {
            u.host.assign(hostport);
        } else {
            u.host.assign(hostport.substr(0, colon));
            port_str = hostport.substr(colon + 1);
        }
    }

    if (!port_str.empty()) {
        if (!parse_port(port_str, u.port)) {
            return u;
        }
    }
    u.ok = !u.host.empty();
    return u;
}

} // namespace detail
} // namespace rtspmech
