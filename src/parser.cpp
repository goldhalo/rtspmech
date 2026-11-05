// =============================================================================
//  parser.cpp —— RTSP 消息解析实现
// =============================================================================
#include "parser.hpp"

#include <rtspmech/common.hpp>

// 必须先包含 rename：pico 的符号在本库里带 rtspmech_ 前缀，
// 这样与 httpmech 各自内嵌一份也不会在链接期撞车。
#include "pico_rename.hpp"

#include <picohttpparser.h>

#include <cstring>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <system_error>

namespace rtspmech {
namespace detail {

bool iequals_ascii(std::string_view a, std::string_view b) noexcept
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

std::string_view find_header(const RtspHeader* headers, std::size_t n,
                             std::string_view name) noexcept
{
    for (std::size_t i = 0; i < n; ++i) {
        if (iequals_ascii(headers[i].name, name)) {
            return headers[i].value;
        }
    }
    return {};
}

std::uint32_t parse_u32(std::string_view s) noexcept
{
    std::uint32_t v = 0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            any = true;
            if (v > (0xFFFFFFFFu - 9u) / 10u) {
                return 0;   // 溢出
            }
            v = v * 10u + static_cast<std::uint32_t>(c - '0');
        } else if (c == ' ' || c == '\t') {
            if (any) {
                break;      // 数字后面允许有空白（如 "1 " ）
            }
        } else {
            break;
        }
    }
    return any ? v : 0;
}

bool parse_message(std::uint8_t* data, std::size_t len,
                   ParsedMessage& out, std::error_code& ec) noexcept
{
    ec.clear();
    out = ParsedMessage{};

    if (data == nullptr || len == 0) {
        ec = make_error_code(RtspErrc::invalid_message);
        return false;
    }

    // phr_header 是 C 结构，先放在栈上，成功后转成 RtspHeader
    phr_header raw[kMaxHeaders];
    std::size_t num_headers = kMaxHeaders;

    // ★ picohttpparser 的 parse_http_version 硬编码校验 "HTTP/"
    //   （见 picohttpparser.c:295-299），RTSP 的 "RTSP/1.0" 会直接判错。
    //
    //   两者都是 4 个字母，原地换掉即可（长度不变、零拷贝）。
    //   注意位置：
    //     请求 → `OPTIONS rtsp://... RTSP/1.0`（在**行尾**）
    //     响应 → `RTSP/1.0 200 OK`            （在**行首**）
    //   所以要在第一行里找，不能只看开头。
    {
        // ★ 只改写**版本 token**，不能瞎找"第一个 RTSP"。
        //
        //   请求行 `SETUP rtsp://host/RTSP-Cam RTSP/1.0` —— URI 里就可能带
        //   "RTSP"。以前从行首往后找第一个出现处替换，会把 URI 改成
        //   "HTTP-Cam"（应用回调里看到的 URI 跟着变坏），同时行尾真正的
        //   "RTSP/1.0" 原封不动 → picohttpparser 判错 → 整条连接被当成
        //   "不可信的流"关掉。
        //
        //   位置其实是确定的：请求行的版本在**行尾**，状态行的在**行首**。
        std::size_t line_end = 0;
        while (line_end + 1 < len
               && !(data[line_end] == '\r' && data[line_end + 1] == '\n')) {
            ++line_end;
        }
        // 容忍行尾空白（有些设备会多打一个空格）
        std::size_t end = line_end;
        while (end > 0 && (data[end - 1] == ' ' || data[end - 1] == '\t')) {
            --end;
        }

        std::size_t at = static_cast<std::size_t>(-1);
        if (end >= 8 && std::memcmp(data + end - 8, "RTSP", 4) == 0) {
            at = end - 8;                          // 请求行：`... RTSP/1.0`
        } else if (len >= 6 && std::memcmp(data, "RTSP", 4) == 0) {
            at = 0;                                // 状态行：`RTSP/1.0 200 OK`
        }
        if (at != static_cast<std::size_t>(-1)) {
            data[at + 0] = 'H';
            data[at + 1] = 'T';
            data[at + 2] = 'T';
            data[at + 3] = 'P';
        }
    }

    const char* buf = reinterpret_cast<const char*>(data);

    // 首 token 以 "HTTP/"（原为 "RTSP/"）开头 → 响应
    const bool is_response = (len >= 5 && std::memcmp(buf, "HTTP/", 5) == 0);

    int consumed = 0;
    if (is_response) {
        int minor = 0;
        int status = 0;
        const char* msg = nullptr;
        std::size_t msg_len = 0;

        consumed = phr_parse_response(buf, len, &minor, &status, &msg, &msg_len,
                                      raw, &num_headers, 0);
        // picohttpparser：-1 = 语法错，-2 = 数据不全。两种都按非法报文处理
        // （能走到这里说明 framer 已经认为一帧完整了，那就是真的错）。
        if (consumed <= 0) {
            ec = make_error_code(RtspErrc::invalid_message);
            return false;
        }

        out.kind    = ParsedMessage::Kind::response;
        out.status  = status;
        out.reason  = (msg != nullptr) ? std::string_view(msg, msg_len) : std::string_view{};
        out.version = "RTSP/1.0";   // picohttpparser 只给 minor，RTSP 目前只有 1.0
    } else {
        const char* method = nullptr;
        std::size_t method_len = 0;
        const char* path = nullptr;
        std::size_t path_len = 0;
        int minor = 0;

        consumed = phr_parse_request(buf, len, &method, &method_len, &path, &path_len,
                                     &minor, raw, &num_headers, 0);
        if (consumed <= 0) {
            ec = make_error_code(RtspErrc::invalid_message);
            return false;
        }

        out.kind    = ParsedMessage::Kind::request;
        out.method  = std::string_view(method, method_len);
        out.uri     = std::string_view(path, path_len);
        out.version = "RTSP/1.0";
    }

    // 转成 RtspHeader（丢弃续行：picohttpparser 用 name==nullptr 表示）
    for (std::size_t i = 0; i < num_headers; ++i) {
        if (raw[i].name == nullptr) {
            continue;   // obs-fold 续行，RTSP 里基本不会出现
        }
        out.headers[out.num_headers].name =
            std::string_view(raw[i].name, raw[i].name_len);
        out.headers[out.num_headers].value =
            std::string_view(raw[i].value, raw[i].value_len);
        ++out.num_headers;
    }

    // body 从 consumed 开始
    const std::size_t consumed_sz = static_cast<std::size_t>(consumed);
    if (consumed_sz < len) {
        out.body     = data + consumed_sz;
        out.body_len = len - consumed_sz;
    }
    return true;
}

} // namespace detail
} // namespace rtspmech
