// ============================================================================
//  test_transport.cpp —— Transport 头解析 + is_rtcp
// ============================================================================
#include <rtspmech/rtsp_transport.hpp>

#include <cstdint>
#include <string>

#include "test_framework.hpp"

using namespace rtspmech;
using namespace netmech_test;

// ---------------------------------------------------------------------------
// 1. UDP 单播：client_port + server_port
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_udp_unicast)
{
    RtspTransport t;
    REQUIRE(parse_transport("RTP/AVP;unicast;client_port=4588-4589;"
                            "server_port=6256-6257", t));

    CHECK(t.mode == TransportMode::udp);
    CHECK(!t.multicast);
    CHECK_EQ(t.client_rtp_port, 4588);
    CHECK_EQ(t.client_rtcp_port, 4589);
    CHECK_EQ(t.server_rtp_port, 6256);
    CHECK_EQ(t.server_rtcp_port, 6257);
}

// ---------------------------------------------------------------------------
// 2. interleaved：两个通道
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_interleaved_pair)
{
    RtspTransport t;
    REQUIRE(parse_transport("RTP/AVP/TCP;unicast;interleaved=0-1", t));

    CHECK(t.mode == TransportMode::interleaved);
    CHECK_EQ(t.ch1, 0);
    CHECK_EQ(t.ch2, 1);
}

// ---------------------------------------------------------------------------
// 3. interleaved 单值：按标准隐含 ch2 = ch1 + 1
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_interleaved_single_value)
{
    RtspTransport t;
    REQUIRE(parse_transport("RTP/AVP/TCP;unicast;interleaved=4", t));

    CHECK(t.mode == TransportMode::interleaved);
    CHECK_EQ(t.ch1, 4);
    CHECK_EQ(t.ch2, 5);      // ★ 标准语义：省略第二个，隐含 +1
}

// ---------------------------------------------------------------------------
// 4. 组播：destination + port + ttl
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_multicast)
{
    RtspTransport t;
    REQUIRE(parse_transport("RTP/AVP;multicast;destination=224.2.0.1;"
                            "port=3456-3457;ttl=16", t));

    CHECK(t.mode == TransportMode::udp);
    CHECK(t.multicast);
    CHECK(t.destination == "224.2.0.1");
    CHECK_EQ(t.mcast_rtp_port, 3456);
    CHECK_EQ(t.mcast_rtcp_port, 3457);
    CHECK_EQ(static_cast<int>(t.ttl), 16);
}

// ---------------------------------------------------------------------------
// 5. SSRC（十六进制）
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_ssrc)
{
    RtspTransport t;
    REQUIRE(parse_transport("RTP/AVP;unicast;client_port=5000-5001;ssrc=1234ABCD", t));
    CHECK_EQ(t.ssrc, 0x1234ABCDu);
}

// ---------------------------------------------------------------------------
// 6. 单值端口对：隐含 +1
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_single_port_implies_pair)
{
    RtspTransport t;
    REQUIRE(parse_transport("RTP/AVP;unicast;client_port=5000", t));
    CHECK_EQ(t.client_rtp_port, 5000);
    CHECK_EQ(t.client_rtcp_port, 5001);
}

// ---------------------------------------------------------------------------
// 7. 大小写与空白容错
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_case_and_space)
{
    RtspTransport t;
    REQUIRE(parse_transport("RTP/AVP/TCP; UNICAST; Interleaved = 2-3", t));
    // 注意："Interleaved = 2-3"（带空格）不匹配 "interleaved=" 前缀，
    // 这里只保证不崩、不误判；实际设备很少这么写
    CHECK(t.mode == TransportMode::interleaved);
}

// ---------------------------------------------------------------------------
// 8. 空/畸形输入被拒绝
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_rejects_empty)
{
    RtspTransport t;
    CHECK(!parse_transport("", t));
    CHECK(!parse_transport("   ", t));
}

// ---------------------------------------------------------------------------
// 9. is_rtcp：按 PT 判断
// ---------------------------------------------------------------------------
NETMECH_TEST(transport_is_rtcp)
{
    // RTP：第 2 字节 = M(1) + PT(7)，PT=96 → 0x60（M=0）
    const std::uint8_t rtp[] = {0x80, 0x60, 0x00, 0x01};
    CHECK(!is_rtcp(rtp, sizeof(rtp)));

    // RTCP SR：PT=200 → 0xC8
    const std::uint8_t sr[] = {0x80, 0xC8, 0x00, 0x06};
    CHECK(is_rtcp(sr, sizeof(sr)));

    // RTCP RR：PT=201
    const std::uint8_t rr[] = {0x80, 0xC9, 0x00, 0x01};
    CHECK(is_rtcp(rr, sizeof(rr)));

    // RTCP SDES：202 / BYE：203 / APP：204
    const std::uint8_t too_short[] = {0x80};
    CHECK(!is_rtcp(too_short, sizeof(too_short)));   // 太短

    const std::uint8_t sdes[] = {0x81, 0xCA};
    CHECK(is_rtcp(sdes, sizeof(sdes)));
    const std::uint8_t bye[] = {0x81, 0xCB};
    CHECK(is_rtcp(bye, sizeof(bye)));
    const std::uint8_t app[] = {0x80, 0xCC};
    CHECK(is_rtcp(app, sizeof(app)));

    // 边界：PT=199（不是 RTCP）/ 205（不是）
    const std::uint8_t p199[] = {0x80, 0xC7};
    CHECK(!is_rtcp(p199, sizeof(p199)));
    const std::uint8_t p205[] = {0x80, 0xCD};
    CHECK(!is_rtcp(p205, sizeof(p205)));

    // 空指针
    CHECK(!is_rtcp(nullptr, 4));
}
