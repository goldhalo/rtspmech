// =============================================================================
//  framer.cpp —— RTSP 流分帧实现
// =============================================================================
#include "framer.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace rtspmech {
namespace detail {

namespace {

constexpr std::uint8_t kDollar  = 0x24;      // '$'
constexpr std::uint8_t kCr       = 0x0D;
constexpr std::uint8_t kLf       = 0x0A;

/// 大小写不敏感的相等比较（只看 ASCII）。
bool iequals(const std::uint8_t* a, std::size_t na, const char* b, std::size_t nb) noexcept
{
    if (na != nb) {
        return false;
    }
    for (std::size_t i = 0; i < na; ++i) {
        const auto ca = static_cast<std::uint8_t>(a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i]);
        const auto cb = static_cast<std::uint8_t>(b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i]);
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

} // namespace

void Framer::append(const std::uint8_t* data, std::size_t len)
{
    if (data == nullptr || len == 0) {
        return;
    }
    // 先压缩再追加，避免缓冲随长连接无限增长
    if (m_pos > 0 && m_pos >= m_buf.size() / 2) {
        compact();
    }
    m_buf.insert(m_buf.end(), data, data + len);
}

void Framer::reset()
{
    m_buf.clear();
    m_pos = 0;
}

void Framer::compact()
{
    if (m_pos == 0) {
        return;
    }
    m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(m_pos));
    m_pos = 0;
}

std::size_t Framer::find_head_end() const noexcept
{
    const std::size_t avail = m_buf.size() - m_pos;
    if (avail < 4) {
        return std::string::npos;
    }
    const std::uint8_t* p = m_buf.data() + m_pos;
    for (std::size_t i = 0; i + 3 < avail; ++i) {
        if (p[i] == kCr && p[i + 1] == kLf && p[i + 2] == kCr && p[i + 3] == kLf) {
            return i;   // 返回相对 m_pos 的偏移
        }
    }
    return std::string::npos;
}

std::size_t Framer::parse_content_length(std::size_t head_end) const noexcept
{
    // 在 [0, head_end) 里逐行找 Content-Length
    const std::uint8_t* p = m_buf.data() + m_pos;
    std::size_t line = 0;

    while (line < head_end) {
        // 一行到下一个 CRLF。
        //
        // ★ 条件是 `eol + 1 <= head_end`，不是 `<`：
        //   head_end 指向 `\r\n\r\n` 的第一个 \r，而**最后一行的 \r 正好等于
        //   head_end**。用 `<` 会让最后一行的 eol 停在 head_end-1，导致行尾丢 1 字节
        //   —— `Content-Length: 39` 会被截成 `Content-Length: 3`，长度算错。
        std::size_t eol = line;
        while (eol + 1 <= head_end && !(p[eol] == kCr && p[eol + 1] == kLf)) {
            ++eol;
        }
        const std::size_t line_len = eol - line;

        static constexpr char kName[] = "content-length";
        constexpr std::size_t kNameLen = sizeof(kName) - 1;

        if (line_len > kNameLen + 1 && p[line + kNameLen] == ':'
            && iequals(p + line, kNameLen, kName, kNameLen)) {
            // 跳过 ':' 与空白，解析数字
            std::size_t i = line + kNameLen + 1;
            while (i < eol && (p[i] == ' ' || p[i] == '\t')) {
                ++i;
            }
            std::size_t value = 0;
            bool has_digit = false;
            for (; i < eol && p[i] >= '0' && p[i] <= '9'; ++i) {
                has_digit = true;
                if (value > (static_cast<std::size_t>(-1) - 9) / 10) {
                    return static_cast<std::size_t>(-1);   // 溢出 → 视为非法
                }
                value = value * 10 + (p[i] - '0');
            }
            return has_digit ? value : static_cast<std::size_t>(-1);
        }

        line = eol + 2;
    }
    return 0;   // 没有这个头 → 0
}

Framer::Status Framer::next(Frame& out)
{
    const std::size_t avail = m_buf.size() - m_pos;
    if (avail == 0) {
        return Status::need_more;
    }

    const std::uint8_t* p = m_buf.data() + m_pos;

    // ---- 二进制帧：$ ch len16 payload ----
    if (p[0] == kDollar) {
        if (avail < 4) {
            return Status::need_more;
        }
        const int channel = p[1];
        const std::size_t body_len =
            (static_cast<std::size_t>(p[2]) << 8) | static_cast<std::size_t>(p[3]);
        const std::size_t total = 4 + body_len;
        if (total > m_max) {
            return Status::too_large;
        }
        if (avail < total) {
            return Status::need_more;
        }

        out.kind    = Frame::Kind::binary;
        out.data    = p;
        out.len     = total;
        out.channel = channel;
        m_pos += total;
        return Status::ok;
    }

    // ---- 文本消息：头部 + Content-Length 字节的 body ----
    const std::size_t head_end = find_head_end();   // 相对 m_pos
    if (head_end == std::string::npos) {
        if (avail > m_max) {
            return Status::too_large;   // 头部一直不结束，肯定异常
        }
        return Status::need_more;
    }

    const std::size_t head_total = head_end + 4;    // 含 \r\n\r\n
    const std::size_t content_length = parse_content_length(head_end);
    if (content_length == static_cast<std::size_t>(-1)) {
        return Status::too_large;      // Content-Length 非法（溢出/非数字）
    }
    if (content_length > m_max) {
        return Status::too_large;
    }

    const std::size_t total = head_total + content_length;
    if (total > m_max) {
        return Status::too_large;
    }
    if (avail < total) {
        return Status::need_more;
    }

    out.kind    = Frame::Kind::text;
    out.data    = p;
    out.len     = total;
    out.channel = -1;
    m_pos += total;
    return Status::ok;
}

} // namespace detail
} // namespace rtspmech
