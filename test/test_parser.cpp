// ============================================================================
//  test_parser.cpp —— RTSP 消息解析测试
// ============================================================================
#include "parser.hpp"

#include <string>
#include <string_view>
#include <cstdint>
#include <system_error>

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

// ---------------------------------------------------------------------------
// URI 里含 "RTSP" 不能被改坏
//
// 解析前要把行尾的 "RTSP/1.0" 原地改成 "HTTP/1.0"（picohttpparser 硬编码校验
// "HTTP/"）。以前是"在行首第一行里找第一个 RTSP 就换"，于是
// `SETUP rtsp://host/RTSP-Cam RTSP/1.0` 会把 **URI** 改成 "HTTP-Cam",
// 行尾真正的版本却原封不动 —— 解析直接失败、连接被当成不可信关掉。
// ---------------------------------------------------------------------------
NETMECH_TEST(parser_uri_containing_rtsp_token)
{
    std::string s =
        "SETUP rtsp://192.168.1.10/RTSP-Cam/track1 RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "\r\n";

    ParsedMessage out;
    REQUIRE(parse_str(s, out));

    CHECK(out.kind == ParsedMessage::Kind::request);
    CHECK(out.method == "SETUP");
    // URI 必须**原样**保留（这是应用要用的地址）
    CHECK_MSG(out.uri == "rtsp://192.168.1.10/RTSP-Cam/track1",
              ("URI 被改坏了: " + std::string(out.uri)).c_str());
    CHECK(out.version == "RTSP/1.0");
}

// ---------------------------------------------------------------------------
// 头注入：头名里的 CR/LF/冒号一律拒绝，值里的 CR/LF 换成空格
//
// 库自己拼报文时更不该留这个口子 —— 本库的 framer 防的就是这类输入。
// ---------------------------------------------------------------------------
NETMECH_TEST(builder_rejects_header_injection)
{
    RequestBuilder b("OPTIONS", "rtsp://h/");
    b.header("X-Ok", "fine");
    b.header("X-Evil\r\nInjected", "v");            // 头名含 CRLF → 整条丢弃
    b.header("X-Colon:Bad", "v");                   // 头名含冒号 → 整条丢弃
    b.header("X-Val", "a\r\nX-Smuggled: 1");        // 值里的 CRLF → 换成空格

    const std::string wire = b.build();

    CHECK_MSG(wire.find("Injected") == std::string::npos, "头名注入没有被挡住");
    CHECK_MSG(wire.find("X-Colon") == std::string::npos, "含冒号的头名应当被拒绝");

    // 值里的 CRLF 变成空格：`a  X-Smuggled: 1` —— 仍在同一行，不会多出一个头
    CHECK_MSG(wire.find("X-Val: a  X-Smuggled: 1") != std::string::npos,
              "值里的 CRLF 应当被替换成空格");
    // 整段报文里只允许出现正常的 `\r\n` 行分隔：以 "X-Val:" 开头的那行不能
    // 变成两行
    const std::size_t pos = wire.find("X-Val: ");
    REQUIRE(pos != std::string::npos);
    CHECK(wire.find("\r\n", pos) == wire.find("  X-Smuggled: 1\r\n", pos) +
                                        std::string("  X-Smuggled: 1").size());
}

// ---------------------------------------------------------------------------
// body() 不能重复补 Content-Length
//
// 调用方自己写过的话，再补一个就成了两个同名头：本库按第一个解析、
// 第三方可能按第二个 —— 那是协议歧义，比少一个头难查得多。
// ---------------------------------------------------------------------------
NETMECH_TEST(builder_does_not_duplicate_content_length)
{
    RequestBuilder b("ANNOUNCE", "rtsp://h/live");
    b.header("Content-Length", "4");     // 手动写
    b.body("abcd");                      // body 不应再补一个

    const std::string wire = b.build();
    std::size_t count = 0;
    for (std::size_t p = wire.find("Content-Length"); p != std::string::npos;
         p = wire.find("Content-Length", p + 1)) {
        ++count;
    }
    CHECK_EQ(count, std::size_t{1});
}
