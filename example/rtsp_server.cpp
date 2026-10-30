// =============================================================================
//  rtsp_server —— 极简 RTSP 服务端（推一路 H.264）
// =============================================================================
//  用法：rtsp_server <h264 文件> [端口]
//  例：  rtsp_server test.h264 8554
//        然后 ffplay rtsp://127.0.0.1:8554/live
//
//  它做什么：
//    * 响应 OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN
//    * DESCRIBE 回一个 H.264 的 SDP（packetization-mode=1）
//    * SETUP 收 interleaved 或 UDP 都接受，统一用 interleaved 发（简单）
//    * PLAY 后按 25fps 循环推流
//
//  它不做什么：
//    * 不做认证、不解析 SDP、不支持暂停/快进、不做 RTCP
//    * 只服务一个客户端（demo 用）
//
//  依赖 rtpmech 的 H264Packetizer（按 RFC 6184 分片）。
// =============================================================================
#include <rtspmech/rtspmech.hpp>

#include <rtpmech/rtp_packet.hpp>
#include <rtpmech/rtcp_packet.hpp>
#include <rtpmech/h265.hpp>
#include <rtpmech/h264.hpp>
#include <rtpmech/span.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace rtspmech;
using namespace netmech;

namespace {

using namespace std::chrono_literals;

constexpr std::uint8_t  kPayloadType     = 96;
constexpr std::uint32_t kSsrc            = 0x0BADCAFE;
constexpr std::size_t   kMaxRtpPacketSize = 1400;
constexpr int          kFps          = 25;
constexpr std::uint32_t kClockRate   = 90000;

const char* kSdpH264 =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=rtspmech demo\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1\r\n"
    "a=control:trackID=0\r\n";

const char* kSdpH265 =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=rtspmech demo\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H265/90000\r\n"
    "a=fmtp:96 sprop-max-don-diff=0\r\n"
    "a=control:trackID=0\r\n";

// -----------------------------------------------------------------------------
//  H264 文件：拆成"帧"（SPS/PPS/SEI 归到后面的第一个 VCL NAL）
// -----------------------------------------------------------------------------

struct Stream {
    std::vector<std::vector<std::uint8_t>> frames;

    /// @param[in] is_h265 true = 按 H.265 的类型判断 VCL；false = H.264
    bool load(const char* path, bool is_h265)
    {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) {
            std::fprintf(stderr, "打不开文件: %s\n", path);
            return false;
        }
        std::vector<std::uint8_t> raw;
        std::uint8_t buf[64 * 1024];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            raw.insert(raw.end(), buf, buf + n);
        }
        std::fclose(f);

        if (raw.empty()) {
            std::fprintf(stderr, "文件为空\n");
            return false;
        }

        // 拆 NAL
        struct Range { std::size_t begin, end; };
        std::vector<Range> nals;
        std::size_t nal_begin = 0;
        bool in_nal = false;
        std::size_t i = 0;
        while (i < raw.size()) {
            std::size_t sc = 0;
            if (i + 4 <= raw.size() && raw[i] == 0 && raw[i+1] == 0 && raw[i+2] == 0 && raw[i+3] == 1) {
                sc = 4;
            } else if (i + 3 <= raw.size() && raw[i] == 0 && raw[i+1] == 0 && raw[i+2] == 1) {
                sc = 3;
            }
            if (sc != 0) {
                if (in_nal && i > nal_begin) { nals.push_back({nal_begin, i}); }
                i += sc;
                nal_begin = i;
                in_nal = true;
            } else {
                ++i;
            }
        }
        if (in_nal && raw.size() > nal_begin) { nals.push_back({nal_begin, raw.size()}); }

        if (nals.empty()) {
            std::fprintf(stderr, "文件里没有 NAL\n");
            return false;
        }

        // ---- 切帧 ----
        //
        // 优先按 **AUD（Access Unit Delimiter）** 切：H.264 是 type 9、
        // H.265 是 type 35。ffmpeg/x265 加 `aud=1` 就会输出。
        //
        // ★ 为什么不能只按 VCL 切
        //   一帧可以由**多个 slice（多个 VCL NAL）**组成，按 VCL 切会切多。
        //   实测：75 帧的 H.265 流被切成了 147 帧，接收端 DTS 重复
        //   （ffmpeg 报 "non monotonically increasing dts"）。
        //
        // 兜底：流里没有 AUD 时退回按 VCL 切（单 slice 的流仍正确）。
        const auto nal_type_of = [is_h265](std::uint8_t b0) -> int {
            return is_h265 ? ((b0 >> 1) & 0x3F) : (b0 & 0x1F);
        };
        const int aud_type = is_h265 ? 35 : 9;
        const auto is_vcl_type = [is_h265](int t) {
            return is_h265 ? (t <= 31) : (t >= 1 && t <= 5);
        };

        bool has_aud = false;
        for (const auto& r : nals) {
            if (r.end > r.begin && nal_type_of(raw[r.begin]) == aud_type) {
                has_aud = true;
                break;
            }
        }

        std::vector<std::uint8_t> pending;
        for (const auto& r : nals) {
            if (r.end <= r.begin) { continue; }
            const int type = nal_type_of(raw[r.begin]);

            if (has_aud) {
                // 遇到 AUD：上一帧结束，开始新帧
                if (type == aud_type && !pending.empty()) {
                    frames.push_back(std::move(pending));
                    pending.clear();
                }
                pending.insert(pending.end(), {0, 0, 0, 1});
                pending.insert(pending.end(), raw.begin() + r.begin, raw.begin() + r.end);
            } else {
                // 没 AUD：按 VCL 切
                pending.insert(pending.end(), {0, 0, 0, 1});
                pending.insert(pending.end(), raw.begin() + r.begin, raw.begin() + r.end);
                if (is_vcl_type(type)) {
                    frames.push_back(std::move(pending));
                    pending.clear();
                }
            }
        }
        if (!pending.empty()) {
            frames.push_back(std::move(pending));
        }

        std::printf("[server] 载入 %zu 帧（%zu 个 NAL，%s%s）\n",
                    frames.size(), nals.size(), is_h265 ? "H.265" : "H.264",
                    has_aud ? "，按 AUD 切" : "，按 VCL 切");
        return !frames.empty();
    }
};

// -----------------------------------------------------------------------------
//  服务端
// -----------------------------------------------------------------------------

class Server final : public Linkable,
                     public RtspHandler,
                     public std::enable_shared_from_this<Server> {
public:
    Server(std::shared_ptr<Stream> stream, bool is_h265)
        : m_stream(std::move(stream)), m_h265(is_h265) {}

    std::shared_ptr<ConnHandler> on_accept(Conn& c) override
    {
        std::error_code ec;
        auto self = shared_from_this();
        auto s = RtspSession::create(c.shared_from_this(), self, ec);
        if (ec) {
            std::fprintf(stderr, "[server] create: %s\n", ec.message().c_str());
            return nullptr;
        }
        m_session = s;
        std::printf("[server] 客户端连上了\n");
        return s;
    }

    void on_close(std::error_code ec) override
    {
        if (ec) {
            std::printf("[server] 客户端断开: %s\n", ec.message().c_str());
        }
        m_playing.store(false);
    }

    void on_request(RtspSession& sess, const RtspRequest& req) override
    {
        std::printf("[server] <- %.*s\n",
                    static_cast<int>(req.method.size()), req.method.data());

        if (req.method == "OPTIONS") {
            reply(sess, req, ResponseBuilder(200, "OK")
                                .header("Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN"));
        } else if (req.method == "DESCRIBE") {
            reply(sess, req, ResponseBuilder(200, "OK")
                                .header("Content-Type", "application/sdp")
                                .header("Content-Base", "rtsp://127.0.0.1/live/")
                                .body(m_h265 ? kSdpH265 : kSdpH264));
        } else if (req.method == "SETUP") {
            // 客户端要什么就给什么；demo 统一用 interleaved 发
            reply(sess, req, ResponseBuilder(200, "OK")
                                .header("Transport",
                                        "RTP/AVP/TCP;unicast;interleaved=0-1")
                                .header("Session", m_session_id));
        } else if (req.method == "PLAY") {
            reply(sess, req, ResponseBuilder(200, "OK")
                                .header("Session", m_session_id)
                                .header("Range", "npt=0.000-"));
            start_stream(sess);
        } else if (req.method == "TEARDOWN") {
            reply(sess, req, ResponseBuilder(200, "OK").header("Session", m_session_id));
            m_playing.store(false);
        } else {
            reply(sess, req, ResponseBuilder(501, "Not Implemented"));
        }
    }

    void request_stop() { m_playing.store(false); }
    bool playing() const { return m_playing.load(); }

private:
    void reply(RtspSession& sess, const RtspRequest& req, ResponseBuilder b)
    {
        b.cseq(req.cseq());
        sess.send_response(b.build());
    }

    void start_stream(RtspSession& sess)
    {
        if (m_playing.exchange(true)) {
            return;   // 已经在推了
        }
        std::printf("[server] PLAY，开始推流（%d fps，%zu 帧循环）\n",
                    kFps, m_stream->frames.size());

        m_session = std::shared_ptr<RtspSession>(&sess, [](RtspSession*) {});  // 不接管生命周期

        auto self = shared_from_this();
        std::thread([self, session = m_session] {
            const auto interval = std::chrono::microseconds(1000000 / kFps);
            auto next = std::chrono::steady_clock::now();
            std::uint32_t ts  = 0;
            std::uint16_t seq = 0;
            std::size_t   idx = 0;
            std::uint32_t sent = 0;

            std::vector<std::uint8_t> rtp(kMaxRtpPacketSize);

            // 两种分片器的接口一致（Packetizer），用一个 lambda 抹平类型
            rtpmech::H264Packetizer h264;
            rtpmech::H265Packetizer h265;
            const auto emit = [&](const std::vector<std::uint8_t>& frame,
                                  std::uint32_t frame_ts) {
                auto& pk = self->m_h265
                                     ? static_cast<rtpmech::Packetizer&>(h265)
                                     : static_cast<rtpmech::Packetizer&>(h264);
                pk.on_frame(
                    rtpmech::span<const std::uint8_t>(frame.data(), frame.size()),
                    [&](rtpmech::span<const std::uint8_t> payload, bool marker) {
                        rtpmech::RtpPacketInfo info;
                        info.payload_type = kPayloadType;
                        info.marker       = marker;
                        info.sequence     = seq++;
                        info.timestamp    = frame_ts;
                        info.ssrc         = kSsrc;

                        std::error_code ec2;
                        const std::size_t n = rtpmech::build_rtp_packet(
                            rtp.data(), rtp.size(), info, payload, ec2);
                        if (ec2 || n == 0) {
                            return;
                        }
                        session->send_interleaved(0, rtp.data(), n);
                        ++sent;
                    });
            };

            auto last_rtcp = std::chrono::steady_clock::now();

            while (self->m_playing.load()) {
                const auto& frame = self->m_stream->frames[idx];
                idx = (idx + 1) % self->m_stream->frames.size();

                const std::uint32_t frame_ts = ts;

                // 一帧 → 1..N 个 RTP payload → 每个 payload 各自组一个 RTP 头
                emit(frame, frame_ts);

                // ---- RTCP SR ----
                //
                // 没有 SR，接收端拿不到 NTP↔RTP 的映射，只能用"包到达时间"
                // 估 PTS，发送节奏一抖 DTS 就不单调（ffmpeg 会报
                // "non monotonically increasing dts"）。RFC 3550 的最短间隔是
                // 5 秒，这里按 5 秒发。
                const auto now = std::chrono::steady_clock::now();
                if (now - last_rtcp >= std::chrono::seconds(5)) {
                    last_rtcp = now;

                    rtpmech::SenderReport sr;
                    sr.ssrc = kSsrc;
                    const auto wall = std::chrono::system_clock::now();
                    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                          wall.time_since_epoch()).count();
                    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           wall.time_since_epoch()).count() % 1000000000LL;
                    const rtpmech::NtpTime ntp =
                        rtpmech::make_ntp_time(secs, static_cast<std::uint32_t>(nanos));
                    sr.ntp_msw       = ntp.seconds;
                    sr.ntp_lsw       = ntp.fraction;
                    sr.rtp_timestamp = frame_ts;
                    sr.packet_count  = sent;
                    sr.octet_count   = sent * 1000;   // 估算值，demo 够用

                    std::vector<std::uint8_t> rtcpBuf(kMaxRtpPacketSize);
                    std::error_code ec3;
                    const std::size_t n3 = rtpmech::build_rtcp_sender_report(
                        rtcpBuf.data(), rtcpBuf.size(), sr, ec3);
                    if (!ec3 && n3 > 0) {
                        // RTCP 在 interleaved 里走通道 1（与 SETUP 的 0-1 对应）
                        session->send_interleaved(1, rtcpBuf.data(), n3);
                    }
                }

                ts += kClockRate / kFps;

                next += interval;
                std::this_thread::sleep_until(next);
            }
            std::printf("[server] 推流结束，共发出 %u 个 RTP 包\n", sent);
        }).detach();
    }

    std::shared_ptr<Stream>      m_stream;
    bool                         m_h265 = false;
    std::shared_ptr<RtspSession> m_session;
    std::string                  m_session_id = "00000001";
    std::atomic<bool>            m_playing{false};
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <h264/h265 文件> [端口] [--h265]\n", argv[0]);
        return 2;
    }
    std::uint16_t port = 8554;
    bool is_h265 = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--h265") == 0) {
            is_h265 = true;
        } else if (std::atoi(argv[i]) > 0) {
            port = static_cast<std::uint16_t>(std::atoi(argv[i]));
        }
    }

    auto stream = std::make_shared<Stream>();
    if (!stream->load(argv[1], is_h265)) {
        return 1;
    }

    if (!netmech::initialize(2)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }

    auto mech   = Mechanism::create();
    auto server = std::make_shared<Server>(stream, is_h265);

    const Address addr{"0.0.0.0", port};
    std::error_code ec;
    mech->set_linkable(server);
    if ((ec = mech->listen(addr, server))) {
        std::fprintf(stderr, "listen: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("[server] 监听 rtsp://127.0.0.1:%u/live（%s）\n",
                port, is_h265 ? "H.265" : "H.264");
    std::printf("[server] 用 ffplay 播放：ffplay rtsp://127.0.0.1:%u/live\n", port);
    std::printf("[server] Ctrl+C 退出\n\n");

    // 简单的主循环：跑 60 秒或直到手动 Ctrl+C
    for (int i = 0; i < 600; ++i) {
        std::this_thread::sleep_for(100ms);
    }

    server->request_stop();
    mech->close_all();
    netmech::shutdown();
    return 0;
}
