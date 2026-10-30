// =============================================================================
//  framer.hpp —— RTSP 流分帧（内部头）
// =============================================================================
//  RTSP 的一个 TCP 连接上混杂两种数据：
//
//      OPTIONS rtsp://... RTSP/1.0\r\nCSeq: 1\r\n\r\n      ← 文本
//      $ <ch> <len16> <payload...>                          ← 二进制（interleaved）
//
//  本类只负责"从字节流里切出完整的一条"，不解释内容。
//  切出来的东西指向内部缓冲，**下一次 append 后失效** —— 调用方必须在
//  下一次 append 之前处理完。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtspmech {
namespace detail {

class Framer {
public:
    /// 切出来的一帧。
    struct Frame {
        enum class Kind {
            text,     ///< 完整的 RTSP 文本消息（含头部与 body）
            binary,   ///< `$` 帧（含 4 字节帧头）
        };

        Kind                kind = Kind::text;
        const std::uint8_t* data = nullptr;   ///< 指向内部缓冲
        std::size_t         len  = 0;
        int                 channel = -1;     ///< binary 专用

        /// 去掉 `$` 帧头后的载荷（仅 binary 有意义）。
        [[nodiscard]] const std::uint8_t* payload() const noexcept
        {
            return kind == Kind::binary ? data + 4 : data;
        }

        [[nodiscard]] std::size_t payload_size() const noexcept
        {
            return kind == Kind::binary ? len - 4 : len;
        }
    };

    /// 追加收到的字节。
    void append(const std::uint8_t* data, std::size_t len);

    /// 一次 next() 的结果。
    enum class Status {
        ok,        ///< 取到一帧
        need_more, ///< 数据不够，等更多字节
        too_large, ///< 单条消息超过上限（或 Content-Length 非法），应当断开连接
    };

    /// 尝试取出一帧。
    Status next(Frame& out);

    /// 丢弃所有缓冲。
    void reset();

    /// 当前缓冲里还有多少没消费的字节。
    [[nodiscard]] std::size_t buffered() const noexcept { return m_buf.size() - m_pos; }

    /// 单条消息的长度上限（默认 2 MB）。
    void set_max_message(std::size_t bytes) noexcept { m_max = bytes; }

private:
    /// 在 `[m_pos, m_pos+avail)` 里找 `\r\n\r\n`，找不到返回 npos。
    [[nodiscard]] std::size_t find_head_end() const noexcept;

    /// 从头部区域解析 Content-Length；没有则返回 0。
    [[nodiscard]] std::size_t parse_content_length(std::size_t head_end) const noexcept;

    /// 把已消费的前缀丢掉，避免缓冲无限增长。
    void compact();

    std::vector<std::uint8_t> m_buf;
    std::size_t               m_pos = 0;
    std::size_t               m_max = 2u * 1024u * 1024u;
};

} // namespace detail
} // namespace rtspmech
