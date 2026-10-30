// ============================================================================
//  test_framer.cpp —— RTSP 流分帧测试
//
//  核心场景：同一段字节流，按不同粒度喂进去，切出来的消息必须完全一致。
//  这类地方最容易出粘包/拆包 bug，所以"按 1 字节喂"是必测项。
// ============================================================================
#include "framer.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "test_framework.hpp"

using namespace rtspmech::detail;
using namespace netmech_test;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes to_bytes(const std::string& s)
{
    return Bytes(s.begin(), s.end());
}

/// 把整段流喂给 framer，收集所有切出的帧（拷贝成独立缓冲，便于比较）。
struct Collected {
    std::vector<std::string>   texts;
    std::vector<int>           channels;
    std::vector<std::string>   binaries;
};

Collected drain(Framer& f)
{
    Collected out;
    Framer::Frame fr;
    while (f.next(fr) == Framer::Status::ok) {
        const char* p = reinterpret_cast<const char*>(fr.data);
        if (fr.kind == Framer::Frame::Kind::binary) {
            out.channels.push_back(fr.channel);
            out.binaries.emplace_back(p, fr.len);
        } else {
            out.texts.emplace_back(p, fr.len);
        }
    }
    return out;
}

/// 按固定块大小喂数据。
Collected feed_chunked(const Bytes& stream, std::size_t chunk)
{
    Framer f;
    Collected out;
    Framer::Frame fr;
    std::size_t off = 0;
    while (off < stream.size()) {
        const std::size_t n = std::min(chunk, stream.size() - off);
        f.append(stream.data() + off, n);
        off += n;
        while (f.next(fr) == Framer::Status::ok) {
            const char* p = reinterpret_cast<const char*>(fr.data);
            if (fr.kind == Framer::Frame::Kind::binary) {
                out.channels.push_back(fr.channel);
                out.binaries.emplace_back(p, fr.len);
            } else {
                out.texts.emplace_back(p, fr.len);
            }
        }
    }
    return out;
}

const std::string kOptions =
    "OPTIONS rtsp://127.0.0.1:554/stream RTSP/1.0\r\n"
    "CSeq: 1\r\n"
    "User-Agent: rtspmech-test\r\n"
    "\r\n";

const std::string kDescribe =
    "RTSP/1.0 200 OK\r\n"
    "CSeq: 2\r\n"
    "Content-Type: application/sdp\r\n"
    "Content-Length: 39\r\n"
    "\r\n"
    "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=test\r\n";   // 正好 38 字节

/// 构造一个 $ 帧。
std::string make_binary(int channel, const std::string& payload)
{
    std::string s;
    s.push_back('$');
    s.push_back(static_cast<char>(channel));
    s.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
    s.push_back(static_cast<char>(payload.size() & 0xFF));
    s += payload;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 单条文本消息
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_single_text)
{
    Framer f;
    const auto b = to_bytes(kOptions);
    f.append(b.data(), b.size());

    auto got = drain(f);
    REQUIRE(got.texts.size() == 1u);
    CHECK(got.texts[0] == kOptions);
    CHECK_EQ(f.buffered(), 0u);
}

// ---------------------------------------------------------------------------
// 2. 粘包：一次到达两条
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_pipelined_messages)
{
    Framer f;
    const auto b = to_bytes(kOptions + kDescribe);
    f.append(b.data(), b.size());

    auto got = drain(f);
    REQUIRE(got.texts.size() == 2u);
    CHECK(got.texts[0] == kOptions);
    CHECK(got.texts[1] == kDescribe);
    CHECK_EQ(f.buffered(), 0u);
}

// ---------------------------------------------------------------------------
// 3. 带 body 的响应：必须等到 Content-Length 字节收满
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_waits_for_body)
{
    Framer f;
    const auto b = to_bytes(kDescribe);

    // 先只喂头部（不含最后几字节 body）
    const std::size_t head_only = kDescribe.size() - 5;
    f.append(b.data(), head_only);

    Framer::Frame fr;
    CHECK(f.next(fr) == Framer::Status::need_more);   // body 还没收满

    f.append(b.data() + head_only, b.size() - head_only);
    CHECK(f.next(fr) == Framer::Status::ok);
    CHECK_EQ(fr.len, kDescribe.size());
}

// ---------------------------------------------------------------------------
// 4. 二进制帧
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_binary_frame)
{
    Framer f;
    const std::string payload = "\x80\x60\x00\x01payload-data";
    const std::string wire = make_binary(0, payload);
    const auto b = to_bytes(wire);
    f.append(b.data(), b.size());

    auto got = drain(f);
    REQUIRE(got.binaries.size() == 1u);
    CHECK_EQ(got.channels[0], 0);
    CHECK_EQ(got.binaries[0].size(), wire.size());
    CHECK_EQ(f.buffered(), 0u);
}

// ---------------------------------------------------------------------------
// 5. 文本与二进制混合（真实 interleaved 的样子）
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_mixed_stream)
{
    const std::string wire =
        kOptions +
        make_binary(0, std::string(100, 'R')) +
        kDescribe +
        make_binary(1, std::string(28, 'C'));

    Framer f;
    const auto b = to_bytes(wire);
    f.append(b.data(), b.size());

    auto got = drain(f);
    CHECK_EQ(got.texts.size(), 2u);
    CHECK_EQ(got.binaries.size(), 2u);
    CHECK_EQ(got.channels[0], 0);
    CHECK_EQ(got.channels[1], 1);
    CHECK_EQ(f.buffered(), 0u);
}

// ---------------------------------------------------------------------------
// 6. ★ 逐字节喂：最能暴露拆包 bug
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_one_byte_at_a_time)
{
    const std::string wire =
        kOptions + make_binary(0, std::string(50, 'x')) + kDescribe;

    const auto whole   = feed_chunked(to_bytes(wire), wire.size());
    const auto by_byte = feed_chunked(to_bytes(wire), 1);

    REQUIRE(by_byte.texts.size() == whole.texts.size());
    REQUIRE(by_byte.binaries.size() == whole.binaries.size());
    CHECK(by_byte.texts == whole.texts);
    CHECK(by_byte.binaries == whole.binaries);
    CHECK(by_byte.channels == whole.channels);
}

// ---------------------------------------------------------------------------
// 7. ★ 各种块大小喂，结果必须一致
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_chunk_sizes_agree)
{
    const std::string wire =
        kOptions + make_binary(0, std::string(200, 'a')) + kDescribe +
        make_binary(1, std::string(28, 'b'));

    const auto ref = feed_chunked(to_bytes(wire), wire.size());
    REQUIRE(ref.texts.size() == 2u);

    for (std::size_t chunk : {1u, 2u, 3u, 7u, 13u, 64u, 127u, 512u}) {
        const auto got = feed_chunked(to_bytes(wire), chunk);
        CHECK_MSG(got.texts == ref.texts,
                  ("块大小 " + std::to_string(chunk) + " 的文本切分与整段喂不一致").c_str());
        CHECK_MSG(got.binaries == ref.binaries,
                  ("块大小 " + std::to_string(chunk) + " 的二进制切分与整段喂不一致").c_str());
    }
}

// ---------------------------------------------------------------------------
// 8. Content-Length 超限 → too_large（不占内存、不卡死）
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_rejects_oversized_content_length)
{
    Framer f;
    f.set_max_message(1024);

    const std::string head =
        "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 999999999\r\n\r\n";
    const auto b = to_bytes(head);
    f.append(b.data(), b.size());

    Framer::Frame fr;
    CHECK(f.next(fr) == Framer::Status::too_large);
}

// ---------------------------------------------------------------------------
// 9. Content-Length 非数字 → too_large
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_rejects_bad_content_length)
{
    Framer f;
    const std::string head =
        "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: abc\r\n\r\n";
    const auto b = to_bytes(head);
    f.append(b.data(), b.size());

    Framer::Frame fr;
    CHECK(f.next(fr) == Framer::Status::too_large);
}

// ---------------------------------------------------------------------------
// 10. 头部一直不结束 → 超过上限就报错，不会无限吃内存
// ---------------------------------------------------------------------------
NETMECH_TEST(framer_rejects_endless_header)
{
    Framer f;
    f.set_max_message(256);

    std::string junk(512, 'A');   // 没有 \r\n\r\n
    const auto b = to_bytes(junk);
    f.append(b.data(), b.size());

    Framer::Frame fr;
    CHECK(f.next(fr) == Framer::Status::too_large);
}
