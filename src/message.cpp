// =============================================================================
//  message.cpp —— RtspRequest / RtspResponse 的访问器与 Builder
// =============================================================================
#include <rtspmech/session.hpp>

#include "parser.hpp"

#include <string>

namespace rtspmech {

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
    m_head.append(method);
    m_head.push_back(' ');
    m_head.append(uri);
    m_head.append(" RTSP/1.0\r\n");
}

RequestBuilder& RequestBuilder::header(std::string_view name, std::string_view value)
{
    // 只接受合法头名，避免拼出畸形报文
    if (name.empty()) {
        return *this;
    }
    m_head.append(name);
    m_head.append(": ");
    m_head.append(value);
    m_head.append("\r\n");
    if (detail::iequals_ascii(name, "CSeq")) {
        m_has_cseq = true;
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
    m_head.append(reason);
    m_head.append("\r\n");
}

ResponseBuilder& ResponseBuilder::header(std::string_view name, std::string_view value)
{
    if (name.empty()) {
        return *this;
    }
    m_head.append(name);
    m_head.append(": ");
    m_head.append(value);
    m_head.append("\r\n");
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
    if (!m_body.empty()) {
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
