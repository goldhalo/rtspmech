// =============================================================================
//  message.cpp —— RtspRequest / RtspResponse 的访问器与 Builder
// =============================================================================
#include <rtspmech/session.hpp>

#include "parser.hpp"

#include <string>
#include <string_view>
#include <cstdint>

namespace rtspmech {

namespace {

/// 请求行 / 状态行里的危险字符处理。
///
/// `header()` 早就防了 header injection（头名拒绝 CR/LF/冒号、值里的 CR/LF
/// 换空格），但**方法名 / URI / reason 这三处一直没防**：
///   RequestBuilder("OPTIONS", "rtsp://h/x\r\nX-Evil: 1")
/// 会拼出两行头。这里统一把 CR/LF/NUL 换成空格 —— 语义上不改变可见字符，
/// 但绝不可能再拼出新的一行。
void append_line_component(std::string& out, std::string_view value)
{
    for (char c : value) {
        out.push_back((c == '\r' || c == '\n' || c == '\0') ? ' ' : c);
    }
}

} // namespace

// =============================================================================
//  RtspRequest
// =============================================================================

std::string_view RtspRequest::header(std::string_view name) const noexcept
{
    return detail::find_header(headers.data(), headers.size(), name);
}

std::uint32_t RtspRequest::cseq() const noexcept
{
    return detail::parse_u32(header("CSeq"));
}

std::string_view RtspRequest::session() const noexcept
{
    const auto s = header("Session");
    const auto semi = s.find(';');       // "Session: 12345678;timeout=60"
    return (semi == std::string_view::npos) ? s : s.substr(0, semi);
}

// =============================================================================
//  RtspResponse
// =============================================================================

std::string_view RtspResponse::header(std::string_view name) const noexcept
{
    return detail::find_header(headers.data(), headers.size(), name);
}

std::uint32_t RtspResponse::cseq() const noexcept
{
    return detail::parse_u32(header("CSeq"));
}

std::string_view RtspResponse::session() const noexcept
{
    const auto s = header("Session");
    const auto semi = s.find(';');
    return (semi == std::string_view::npos) ? s : s.substr(0, semi);
}

std::string_view RtspResponse::www_authenticate() const noexcept
{
    return header("WWW-Authenticate");
}

// =============================================================================
//  RequestBuilder
// =============================================================================

RequestBuilder::RequestBuilder(std::string_view method, std::string_view uri)
{
    m_head.reserve(128);
    append_line_component(m_head, method);
    m_head.push_back(' ');
    append_line_component(m_head, uri);
    m_head.append(" RTSP/1.0\r\n");
}

RequestBuilder& RequestBuilder::header(std::string_view name, std::string_view value)
{
    // ★ 头名必须是 token（RFC 7230）：空、含 CR/LF、含冒号一律拒绝。
    //   这是 header injection 的入口 —— 本库自己的 framer 就是被这类输入
    //   利用过，库主动拼报文时更不该留这个口子。
    if (name.empty() || name.find_first_of("\r\n:") != std::string_view::npos) {
        return *this;
    }
    // ★ 值里的 CR/LF 换成空格而不是丢弃整条头：值语义还在（多行头本来也
    //   不该出现在这里），但绝不会再拼出一行新的头。
    std::string scratch;
    if (value.find_first_of("\r\n") != std::string_view::npos) {
        scratch.assign(value);
        for (char& c : scratch) {
            if (c == '\r' || c == '\n') {
                c = ' ';
            }
        }
        value = std::string_view(scratch);
    }

    m_head.append(name);
    m_head.append(": ");
    m_head.append(value);
    m_head.append("\r\n");
    if (detail::iequals_ascii(name, "CSeq")) {
        m_has_cseq = true;
    } else if (detail::iequals_ascii(name, "Content-Length")) {
        m_has_content_length = true;
    }
    return *this;
}

RequestBuilder& RequestBuilder::cseq(std::uint32_t seq)
{
    m_head.append("CSeq: ");
    m_head.append(std::to_string(seq));
    m_head.append("\r\n");
    m_has_cseq = true;
    return *this;
}

RequestBuilder& RequestBuilder::body(std::string_view data)
{
    m_body.assign(data.data(), data.size());
    // ★ 必须自动补 Content-Length：ANNOUNCE 带 SDP 时没有它，接收端的
    //   framer 就无法确定 body 长度，会把 SDP 当成下一条消息的开头，
    //   然后解析失败、断开连接。
    if (!m_body.empty() && !m_has_content_length) {
        header("Content-Length", std::to_string(m_body.size()));
    }
    return *this;
}

void RequestBuilder::assign_cseq_if_absent(std::uint32_t seq)
{
    if (!m_has_cseq) {
        cseq(seq);
    }
}

std::string RequestBuilder::build() const
{
    std::string out;
    out.reserve(m_head.size() + m_body.size() + 2);
    out.append(m_head);
    out.append("\r\n");
    out.append(m_body);
    return out;
}

// =============================================================================
//  ResponseBuilder
// =============================================================================

ResponseBuilder::ResponseBuilder(int status, std::string_view reason)
{
    m_head.reserve(128);
    m_head.append("RTSP/1.0 ");
    m_head.append(std::to_string(status));
    m_head.push_back(' ');
    append_line_component(m_head, reason);   // reason 同样不能带 CRLF
    m_head.append("\r\n");
}

ResponseBuilder& ResponseBuilder::header(std::string_view name, std::string_view value)
{
    // 同 RequestBuilder：头名拒绝 CR/LF/冒号，值里的 CR/LF 换成空格。
    if (name.empty() || name.find_first_of("\r\n:") != std::string_view::npos) {
        return *this;
    }
    std::string scratch;
    if (value.find_first_of("\r\n") != std::string_view::npos) {
        scratch.assign(value);
        for (char& c : scratch) {
            if (c == '\r' || c == '\n') {
                c = ' ';
            }
        }
        value = std::string_view(scratch);
    }

    m_head.append(name);
    m_head.append(": ");
    m_head.append(value);
    m_head.append("\r\n");
    if (detail::iequals_ascii(name, "Content-Length")) {
        m_has_content_length = true;
    }
    return *this;
}

ResponseBuilder& ResponseBuilder::cseq(std::uint32_t seq)
{
    m_head.append("CSeq: ");
    m_head.append(std::to_string(seq));
    m_head.append("\r\n");
    return *this;
}

ResponseBuilder& ResponseBuilder::body(std::string_view data)
{
    m_body.assign(data.data(), data.size());
    // 调用方自己写过 Content-Length 就不再补 —— 两个同名头是协议歧义。
    if (!m_body.empty() && !m_has_content_length) {
        header("Content-Length", std::to_string(m_body.size()));
    }
    return *this;
}

std::string ResponseBuilder::build() const
{
    std::string out;
    out.reserve(m_head.size() + m_body.size() + 2);
    out.append(m_head);
    out.append("\r\n");
    out.append(m_body);
    return out;
}

} // namespace rtspmech
