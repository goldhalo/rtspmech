// =============================================================================
//  rtsp_pusher —— 推流客户端
// =============================================================================
//  用法：rtsp_pusher <relay地址> <h264文件> [路数] [--tcp|--udp]
//
//  例：  rtsp_pusher rtsp://127.0.0.1:8554 test.h264 100 --tcp
//
//  做三件事：
//    1. 起 N 个 RtspClient（推流模式，URL 各不相同：/push/0 ... /push/N-1）
//    2. 每个 RECORD 成功后，按 25fps 循环发这一路的 H264
//    3. 50 路用 interleaved（TCP），50 路用 UDP
//
//  ★ 帧数据共享
//  H264 文件只加载一次、分片也只做一次，100 路共用同一份 payload。
//  每路只维护自己的 seq / ts / ssrc。
// =============================================================================
#include <rtspmech/client.hpp>
#include <rtspmech/rtspmech.hpp>

#include <rtpmech/h264.hpp>
#include <rtpmech/rtp_packet.hpp>

#include <netmech/net.hpp>
#include <netmech/udp.hpp>

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

constexpr int           kFps        = 25;
constexpr std::uint32_t kClockRate  = 90000;
constexpr std::uint8_t  kPayloadType = 96;
constexpr std::size_t   kMaxRtp      = 1400;

/// 共享的一帧（已分好片）。
struct SharedFrame {
    std::vector<std::vector<std::uint8_t>> payloads;   ///< 每片一个 RTP payload
    std::vector<bool>                      markers;    ///< 每片是否帧尾
};

/// 加载 H264 文件并预分片（只做一次，100 路共用）。
struct SharedStream {
    std::vector<SharedFrame> frames;
    std::string              sdp;

    bool load(const char* path)
    {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) {
            std::fprintf(stderr, "打不开 %s\n", path);
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
            return false;
        }

        // 按 AUD / VCL 切帧（同 rtsp_server 的做法）
        struct Range { std::size_t b, e; };
        std::vector<Range> nals;
        std::size_t begin = 0;
        bool in = false;
        std::size_t i = 0;
        while (i < raw.size()) {
            std::size_t sc = 0;
            if (i + 4 <= raw.size() && !std::memcmp(&raw[i], "\0\0\0\1", 4)) { sc = 4; }
            else if (i + 3 <= raw.size() && !std::memcmp(&raw[i], "\0\0\1", 3)) { sc = 3; }
            if (sc) {
                if (in && i > begin) { nals.push_back({begin, i}); }
                i += sc;
                begin = i;
                in = true;
            } else {
                ++i;
            }
        }
        if (in && raw.size() > begin) { nals.push_back({begin, raw.size()}); }
        if (nals.empty()) {
            return false;
        }

        bool has_aud = false;
        for (const auto& r : nals) {
            if (r.e > r.b && (raw[r.b] & 0x1F) == 9) { has_aud = true; break; }
        }

        // 提取 SPS(7) / PPS(8)，供"每帧都带上"用
        std::vector<std::uint8_t> sps_pps;
        for (const auto& r : nals) {
            const int t = raw[r.b] & 0x1F;
            if (t == 7 || t == 8) {
                sps_pps.insert(sps_pps.end(), {0, 0, 0, 1});
                sps_pps.insert(sps_pps.end(), raw.begin() + r.b, raw.begin() + r.e);
            }
        }

        rtpmech::H264Packetizer pkt;
        std::vector<std::uint8_t> pending;

        auto flush = [&](std::vector<std::uint8_t>&& data) {
            if (data.empty()) { return; }
            // ★ 每一帧都带上 SPS/PPS。
            //
            //   为什么：relay 的订阅者（ffmpeg）是**随时加入**的，如果只在
            //   开头（或只在 IDR 前）发一次参数集，中途加入的客户端拿到的是
            //   没有 SPS/PPS 的裸片，解码器会报 "non-existing PPS 0"。
            //
            //   代价：每帧多 30 字节左右（SPS ~20 + PPS ~8），25fps 下
            //   带宽增加约 6 kbps，可以忽略。
            std::vector<std::uint8_t> with_ps;
            with_ps.reserve(sps_pps.size() + data.size());
            with_ps.insert(with_ps.end(), sps_pps.begin(), sps_pps.end());
            with_ps.insert(with_ps.end(), data.begin(), data.end());

            SharedFrame f;
            pkt.on_frame(rtpmech::span<const std::uint8_t>(with_ps.data(), with_ps.size()),
                         [&f](rtpmech::span<const std::uint8_t> p, bool m) {
                             f.payloads.emplace_back(p.begin(), p.end());
                             f.markers.push_back(m);
                         });
            if (!f.payloads.empty()) {
                frames.push_back(std::move(f));
            }
        };

        for (const auto& r : nals) {
            const int type = raw[r.b] & 0x1F;
            if (has_aud) {
                if (type == 9 && !pending.empty()) {
                    flush(std::move(pending));
                    pending.clear();
                }
                pending.insert(pending.end(), {0, 0, 0, 1});
                pending.insert(pending.end(), raw.begin() + r.b, raw.begin() + r.e);
            } else {
                pending.insert(pending.end(), {0, 0, 0, 1});
                pending.insert(pending.end(), raw.begin() + r.b, raw.begin() + r.e);
                if (type >= 1 && type <= 5) {
                    flush(std::move(pending));
                    pending.clear();
                }
            }
        }
        if (!pending.empty()) {
            flush(std::move(pending));
        }

        sdp =
            "v=0\r\n"
            "o=- 0 0 IN IP4 127.0.0.1\r\n"
            "s=push\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1\r\n"
            "a=control:trackID=0\r\n";

        std::printf("[pusher] 载入 %zu 帧，共 %zu 个 RTP payload\n",
                    frames.size(),
                    [&] {
                        std::size_t t = 0;
                        for (const auto& f : frames) { t += f.payloads.size(); }
                        return t;
                    }());
        return !frames.empty();
    }
};

/// 一路推流的状态。
struct PushChannel : public ClientHandler,
                     public std::enable_shared_from_this<PushChannel> {
    int                        index = 0;
    bool                       use_udp = false;
    std::shared_ptr<RtspClient> client;
    std::shared_ptr<SharedStream> stream;

    std::uint16_t seq  = 0;
    std::uint32_t ts   = 0;
    std::size_t   idx  = 0;
    std::uint32_t ssrc = 0;
    std::string   relay_ip;
    std::shared_ptr<netmech::UdpEndpoint> udp;
    std::atomic<bool> recording{false};
    std::atomic<std::uint64_t> sent{0};

    void on_recording(RtspClient& c) override
    {
        recording.store(true);
        std::printf("[pusher] #%d RECORD 成功（%s）\n", index, use_udp ? "UDP" : "TCP");
        (void)c;
    }

    void on_failed(RtspClient&, const char* stage, std::error_code ec, int status) override
    {
        std::fprintf(stderr, "[pusher] #%d %s 失败: %s (status=%d)\n",
                     index, stage, ec ? ec.message().c_str() : "bad status", status);
    }

    /// 按当前帧率发一帧。
    void tick()
    {
        if (!recording.load() || !client) {
            return;
        }
        RtspSession* sess = client->session();
        if (!sess) {
            return;
        }

        const SharedFrame& f = stream->frames[idx];
        idx = (idx + 1) % stream->frames.size();

        const std::uint32_t frame_ts = ts;
        std::vector<std::uint8_t> rtp(kMaxRtp);

        for (std::size_t i = 0; i < f.payloads.size(); ++i) {
            rtpmech::RtpPacketInfo info;
            info.payload_type = kPayloadType;
            info.marker       = f.markers[i];
            info.sequence     = seq++;
            info.timestamp    = frame_ts;
            info.ssrc         = ssrc;

            std::error_code ec;
            const std::size_t n = rtpmech::build_rtp_packet(
                rtp.data(), rtp.size(), info,
                rtpmech::span<const std::uint8_t>(f.payloads[i].data(),
                                                  f.payloads[i].size()),
                ec);
            if (ec || n == 0) {
                continue;
            }

            if (use_udp) {
                const std::uint16_t sport = client->server_udp_port();
                if (udp && sport != 0) {
                    // ★ send_to 是异步的，ConstBuf 的 owner 为空时库不持有数据。
                    //   rtp 是栈上的复用缓冲，下一帧就被覆盖 —— 必须给 owner。
                    auto holder = std::make_shared<std::vector<std::uint8_t>>(
                        rtp.begin(), rtp.begin() + static_cast<std::ptrdiff_t>(n));
                    udp->send_to(netmech::Address{relay_ip, sport},
                                 netmech::ConstBuf{holder, holder->data(),
                                                   holder->size()});
                }
            } else {
                sess->send_interleaved(client->channel(), rtp.data(), n);
            }
            sent.fetch_add(1);
        }

        // ★ 时间戳跨轮累加，不回 0（回绕继续，RTP 允许）
        ts += kClockRate / kFps;
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <relay rtsp://host:port> <h264 文件> [路数=100] [--tcp|--udp|--mixed]\n",
                     argv[0]);
        return 2;
    }
    const std::string base = argv[1];
    const char*       file = argv[2];
    const int         channels = (argc > 3) ? std::atoi(argv[3]) : 100;

    int tcp_count = 0;
    int udp_count = 0;
    const std::string mode = (argc > 4) ? argv[4] : "--mixed";
    if (mode == "--tcp")        { tcp_count = channels; }
    else if (mode == "--udp")   { udp_count = channels; }
    else                        { tcp_count = channels / 2; udp_count = channels - tcp_count; }

    if (!netmech::initialize(4)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }

    auto stream = std::make_shared<SharedStream>();
    if (!stream->load(file)) {
        return 1;
    }

    std::printf("[pusher] 起 %d 路（TCP %d + UDP %d）\n", channels, tcp_count, udp_count);

    // 从 base URL 里取 relay 的 IP（UDP 发 RTP 要用）
    std::string relay_ip = "127.0.0.1";
    {
        constexpr std::string_view prefix = "rtsp://";
        if (base.size() > prefix.size() && base.substr(0, prefix.size()) == prefix) {
            std::string_view rest(base.data() + prefix.size(), base.size() - prefix.size());
            const auto slash = rest.find('/');
            std::string_view hp = (slash == std::string_view::npos)
                                      ? rest
                                      : rest.substr(0, slash);
            const auto colon = hp.rfind(':');
            relay_ip = std::string((colon == std::string_view::npos) ? hp
                                                                     : hp.substr(0, colon));
        }
    }

    std::vector<std::shared_ptr<PushChannel>> chans;
    chans.reserve(static_cast<std::size_t>(channels));

    for (int i = 0; i < channels; ++i) {
        auto ch = std::make_shared<PushChannel>();
        ch->index  = i;
        ch->use_udp = (i >= tcp_count);
        ch->stream   = stream;
        ch->relay_ip = relay_ip;
        ch->ssrc     = 0x10000000u + static_cast<std::uint32_t>(i);

        std::error_code ec;
        if (ch->use_udp) {
            // UDP 推流：一个端点发所有 RTP（bind 到临时端口）
            struct NullHandler : netmech::UdpHandler {
                void on_read(netmech::UdpEndpoint&, std::error_code, netmech::Buf,
                             std::size_t, const netmech::Address&) override {}
            };
            netmech::UdpConfig ucfg;
            ucfg.bind = netmech::Address::any(0);
            ucfg.recv_capacity = 2048;
            ch->udp = netmech::create_udp_endpoint(ucfg, std::make_shared<NullHandler>(),
                                                   nullptr, ec);
            if (!ch->udp) {
                std::fprintf(stderr, "[pusher] #%d UDP 端点创建失败: %s\n",
                             i, ec.message().c_str());
                continue;
            }
        }

        RtspClient::Config cfg;
        cfg.mode      = ClientMode::push;
        cfg.url       = base + "/push/" + std::to_string(i);
        cfg.sdp       = stream->sdp;
        cfg.transport = ch->use_udp ? TransportPreference::udp
                                    : TransportPreference::interleaved;
        cfg.channel   = 0;

        ch->client = RtspClient::create(cfg, ch, ec);
        if (!ch->client) {
            std::fprintf(stderr, "[pusher] #%d 创建失败: %s\n", i, ec.message().c_str());
            continue;
        }
        chans.push_back(ch);
    }

    std::printf("[pusher] 已发起 %zu 路，等 RECORD...\n", chans.size());

    // 25fps 发帧
    const auto interval = std::chrono::microseconds(1000000 / kFps);
    auto next = std::chrono::steady_clock::now();
    std::uint64_t total = 0;

    for (int tick = 0; tick < kFps * 3600; ++tick) {   // 跑一小时
        for (auto& ch : chans) {
            ch->tick();
        }
        next += interval;
        std::this_thread::sleep_until(next);

        if (tick % (kFps * 5) == 0 && tick > 0) {
            for (auto& ch : chans) { total += ch->sent.exchange(0); }
            std::printf("[pusher] %ds：累计发出 %llu 个 RTP 包\n",
                        tick / kFps, static_cast<unsigned long long>(total));
            std::fflush(stdout);
        }
    }

    for (auto& ch : chans) {
        if (ch->client) { ch->client->teardown(); }
    }
    netmech::shutdown();
    return 0;
}
