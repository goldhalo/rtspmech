// ============================================================================
//  test_parser.cpp —— RTSP 消息解析测试
// ============================================================================
#include "parser.hpp"

#include <string>

#include <rtspmech/session.hpp>

#include "test_framework.hpp"

using namespace rtspmech;
using namespace rtspmech::detail;
using namespace netmech_test;

namespace {

/// @param[in,out] s 可写缓冲，**必须由调用方持有到用完 out 之后**
///                  （out 里的 string_view 指向它）
bool parse_str(std::string& s, ParsedMessage& out)
{
    std::error_code ec;
    const bool ok = parse_message(reinterpret_cast<std::uint8_t*>(&s[0]),
                                  s.size(), out, ec);
    return ok && !ec;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 请求
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_request_basic)
{
    std::string s =
        "OPTIONS rtsp://192.168.1.10:554/stream RTSP/1.0\r\n"
        "CSeq: 7\r\n"
        "User-Agent: rtspmech\r\n"
        "\r\n";

    ParsedMessage pm;
    REQUIRE(parse_str(s, pm));
    CHECK(pm.kind == ParsedMessage::Kind::request);
    CHECK(pm.method == "OPTIONS");
    CHECK(pm.uri == "rtsp://192.168.1.10:554/stream");
    CHECK_EQ(pm.num_headers, 2u);
    CHECK_EQ(pm.body_len, 0u);

    CHECK(find_header(pm.headers, pm.num_headers, "CSeq") == "7");
    CHECK(find_header(pm.headers, pm.num_headers, "cseq") == "7");   // 大小写不敏感
    CHECK(find_header(pm.headers, pm.num_headers, "Nonexistent").empty());
}

// ---------------------------------------------------------------------------
// 2. 响应 + SDP body
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_response_with_sdp)
{
    std::string body = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\n";
    std::string s =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 2\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    ParsedMessage pm;
    REQUIRE(parse_str(s, pm));
    CHECK(pm.kind == ParsedMessage::Kind::response);
    CHECK_EQ(pm.status, 200);
    CHECK(pm.reason == "OK");
    CHECK_EQ(pm.body_len, body.size());
    REQUIRE(pm.body != nullptr);
    CHECK(std::string(reinterpret_cast<const char*>(pm.body), pm.body_len) == body);
}

// ---------------------------------------------------------------------------
// 3. Session 头：去掉 ;timeout= 后缀
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_session_strips_timeout)
{
    std::string s =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 3\r\n"
        "Session: 12345678;timeout=60\r\n"
        "\r\n";

    ParsedMessage pm;
    REQUIRE(parse_str(s, pm));

    RtspResponse resp;
    resp.version = pm.version;
    resp.status  = pm.status;
    resp.reason  = pm.reason;
    resp.headers = span<const RtspHeader>(pm.headers, pm.num_headers);
    resp.body    = span<const std::uint8_t>(pm.body, pm.body_len);

    CHECK(resp.session() == "12345678");
    CHECK_EQ(resp.cseq(), 3u);
}

// ---------------------------------------------------------------------------
// 4. 401 + WWW-Authenticate
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_401_digest)
{
    std::string s =
        "RTSP/1.0 401 Unauthorized\r\n"
        "CSeq: 1\r\n"
        "WWW-Authenticate: Digest realm=\"IP Camera\", nonce=\"abc123\"\r\n"
        "\r\n";

    ParsedMessage pm;
    REQUIRE(parse_str(s, pm));
    CHECK_EQ(pm.status, 401);

    RtspResponse resp;
    resp.status  = pm.status;
    resp.headers = span<const RtspHeader>(pm.headers, pm.num_headers);

    const auto auth = resp.www_authenticate();
    CHECK(auth.find("Digest") != std::string_view::npos);
    CHECK(auth.find("nonce=\"abc123\"") != std::string_view::npos);
}

// ---------------------------------------------------------------------------
// 5. RequestBuilder → parse 往返
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_roundtrip_request_builder)
{
    RequestBuilder b("DESCRIBE", "rtsp://127.0.0.1/stream");
    b.header("Accept", "application/sdp");
    b.assign_cseq_if_absent(42);

    std::string wire = b.build();

    ParsedMessage pm;
    REQUIRE(parse_str(wire, pm));
    CHECK(pm.method == "DESCRIBE");
    CHECK(pm.uri == "rtsp://127.0.0.1/stream");
    CHECK(find_header(pm.headers, pm.num_headers, "CSeq") == "42");
    CHECK(find_header(pm.headers, pm.num_headers, "Accept") == "application/sdp");
}

// ---------------------------------------------------------------------------
// 6. assign_cseq_if_absent 不覆盖已设的值
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_cseq_not_overridden)
{
    RequestBuilder b("OPTIONS", "rtsp://x/");
    b.cseq(100);
    b.assign_cseq_if_absent(999);      // 不应生效
    CHECK(b.has_cseq());

    std::string wire = b.build();
    ParsedMessage pm;
    REQUIRE(parse_str(wire, pm));
    CHECK(find_header(pm.headers, pm.num_headers, "CSeq") == "100");
}

// ---------------------------------------------------------------------------
// 7. ResponseBuilder → parse 往返（body 自动带 Content-Length）
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_roundtrip_response_builder)
{
    ResponseBuilder b(200, "OK");
    b.cseq(5);
    b.body("v=0\r\n");

    std::string wire = b.build();

    ParsedMessage pm;
    REQUIRE(parse_str(wire, pm));
    CHECK_EQ(pm.status, 200);
    CHECK(find_header(pm.headers, pm.num_headers, "Content-Length") == "5");
    CHECK_EQ(pm.body_len, 5u);
}

// ---------------------------------------------------------------------------
// 8. 畸形消息被拒绝
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_rejects_garbage)
{
    ParsedMessage pm;
    std::error_code ec;

    std::string junk = "not a valid rtsp message at all\r\n\r\n";
    CHECK(!parse_message(reinterpret_cast<std::uint8_t*>(&junk[0]),
                         junk.size(), pm, ec));
    CHECK(static_cast<bool>(ec));
}
