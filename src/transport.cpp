// =============================================================================
//  transport.cpp —— Transport 头解析 + interleaved 载荷类型判断
// =============================================================================
#include <rtspmech/rtsp_transport.hpp>

#include <cstring>
#include <string_view>

namespace rtspmech {

namespace {

constexpr std::string_view kDefaultTransport = "RTP/AVP";
constexpr std::string_view kTcpTransport     = "RTP/AVP/TCP";

std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/// 解析十进制端口（1~65535）。失败返回 false。
bool parse_port(std::string_view s, std::uint16_t& out) noexcept
{
    if (s.empty() || s.size() > 5) {
        return false;
    }
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v == 0 || v > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

/// 解析 `a-b` 或单个 `a`。单值时 b = a + 1（RTP/RTCP 成对的惯例）。
bool parse_port_pair(std::string_view s, std::uint16_t& a, std::uint16_t& b) noexcept
{
    const auto dash = s.find('-');
    if (dash == std::string_view::npos) {
        if (!parse_port(s, a)) {
            return false;
        }
        b = (a == 65535) ? 0 : static_cast<std::uint16_t>(a + 1);
        return true;
    }
    return parse_port(trim(s.substr(0, dash)), a) &&
           parse_port(trim(s.substr(dash + 1)), b);
}

/// 解析 `a-b` 或单个 `a`（interleaved 通道号，允许 0）。
bool parse_channel_pair(std::string_view s, int& c1, int& c2) noexcept
{
    const auto dash = s.find('-');
    const auto to_int = [](std::string_view x, int& out) {
        x = trim(x);
        if (x.empty() || x.size() > 3) {
            return false;
        }
        int v = 0;
        for (char c : x) {
            if (c < '0' || c > '9') {
                return false;
            }
            v = v * 10 + (c - '0');
        }
        if (v > 255) {
            return false;
        }
        out = v;
        return true;
    };

    if (dash == std::string_view::npos) {
        if (!to_int(s, c1)) {
            return false;
        }
        // ★ 标准语义：单值 = 省略第二个，隐含 +1（RFC 2326 §12.39）
        c2 = (c1 == 255) ? c1 : c1 + 1;
        return true;
    }
    return to_int(s.substr(0, dash), c1) && to_int(s.substr(dash + 1), c2);
}

/// 解析十六进制 SSRC。
bool parse_ssrc(std::string_view s, std::uint32_t& out) noexcept
{
    s = trim(s);
    if (s.empty() || s.size() > 8) {
        return false;
    }
    std::uint32_t v = 0;
    for (char c : s) {
        int d = 0;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return false;
        }
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    out = v;
    return true;
}

} // namespace

bool parse_transport(std::string_view value, RtspTransport& out) noexcept
{
    out = RtspTransport{};

    value = trim(value);
    if (value.empty()) {
        return false;
    }

    // 按 ';' 分段
    std::size_t pos = 0;
    bool first = true;

    while (pos <= value.size()) {
        const auto semi = value.find(';', pos);
        std::string_view tok =
            (semi == std::string_view::npos) ? value.substr(pos) : value.substr(pos, semi - pos);
        tok = trim(tok);

        if (first) {
            first = false;
            // RTP/AVP | RTP/AVP/TCP | RTP/AVP/UDP
            if (iequals(tok, kTcpTransport)) {
                out.mode = TransportMode::interleaved;
            } else if (iequals(tok, "RTP/AVP") || iequals(tok, "RTP/AVP/UDP")) {
                out.mode = TransportMode::udp;
            } else if (tok.empty()) {
                return false;   // 连传输类型都没有，无法判断
            } else {
                // 其它 profile（如 RTP/SAVP）也按 UDP 处理，不拒绝——
                // 设备会发出来，能跑就行，加密与否不是本层能判断的
                out.mode = TransportMode::udp;
            }
        } else if (iequals(tok, "unicast")) {
            out.multicast = false;
        } else if (iequals(tok, "multicast")) {
            out.multicast = true;
        } else if (tok.substr(0, 12) == "client_port=") {
            parse_port_pair(tok.substr(12), out.client_rtp_port, out.client_rtcp_port);
        } else if (tok.substr(0, 12) == "server_port=") {
            parse_port_pair(tok.substr(12), out.server_rtp_port, out.server_rtcp_port);
        } else if (tok.substr(0, 5) == "port=") {
            // 组播的端口
            parse_port_pair(tok.substr(5), out.mcast_rtp_port, out.mcast_rtcp_port);
        } else if (tok.substr(0, 12) == "interleaved=") {
            parse_channel_pair(tok.substr(12), out.ch1, out.ch2);
        } else if (tok.substr(0, 5) == "ssrc=") {
            parse_ssrc(tok.substr(5), out.ssrc);
        } else if (tok.substr(0, 4) == "ttl=") {
            std::string_view t = trim(tok.substr(4));
            unsigned v = 0;
            bool any = false;
            for (char c : t) {
                if (c < '0' || c > '9') { any = false; break; }
                any = true;
                v = v * 10 + static_cast<unsigned>(c - '0');
                if (v > 255) { any = false; break; }
            }
            if (any) {
                out.ttl = static_cast<std::uint8_t>(v);
            }
        } else if (tok.substr(0, 12) == "destination=") {
            out.destination = tok.substr(12);
        } else if (tok.substr(0, 7) == "source=") {
            out.source = tok.substr(7);
        }
        // 其它参数（mode=PLAY、append 等）忽略

        if (semi == std::string_view::npos) {
            break;
        }
        pos = semi + 1;
    }

    return true;
}

bool is_rtcp(const std::uint8_t* data, std::size_t len) noexcept
{
    if (data == nullptr || len < 2) {
        return false;
    }
    // RTP 第 2 字节 = M(1) + PT(7)，PT ≤ 127
    // RTCP 第 2 字节 = PT(8)，200~204
    const std::uint8_t pt = data[1];
    return pt >= 200 && pt <= 204;
}

} // namespace rtspmech
