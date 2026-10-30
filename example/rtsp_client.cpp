// =============================================================================
//  rtsp_client —— 对着真实 RTSP 服务端拉一路流
// =============================================================================
//  用法：rtsp_client <rtsp://host[:port]/path> [--timeout=秒]
//  例：  rtsp_client rtsp://127.0.0.1:8554/live
//
//  流程：OPTIONS → DESCRIBE（拿 SDP）→ SETUP（每个 track 一次）→ PLAY
//  之后收到的 interleaved 数据按 is_rtcp() 分流并统计。
//
//  这是**协议流程演示**，不做解码播放。要看画面请用 VLC/ffplay。
// =============================================================================
#include <rtspmech/rtspmech.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rtspmech;
using namespace netmech;

namespace {

using namespace std::chrono_literals;

/// 把 rtsp://host[:port]/path 拆开。
struct Url {
    std::string host;
    std::uint16_t port = 554;
    std::string path;
    bool ok = false;
};

Url parse_url(std::string_view s)
{
    Url u;
    constexpr std::string_view prefix = "rtsp://";
    if (s.substr(0, prefix.size()) != prefix) {
        return u;
    }
    s.remove_prefix(prefix.size());

    const auto slash = s.find('/');
    std::string_view hostport = (slash == std::string_view::npos) ? s : s.substr(0, slash);
    u.path = (slash == std::string_view::npos) ? "/" : std::string(s.substr(slash));

    const auto colon = hostport.rfind(':');
    if (colon == std::string_view::npos) {
        u.host = std::string(hostport);
    } else {
        u.host = std::string(hostport.substr(0, colon));
        const auto p = hostport.substr(colon + 1);
        unsigned v = 0;
        for (char c : p) {
            if (c < '0' || c > '9') { return u; }
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (v == 0 || v > 65535) { return u; }
        u.port = static_cast<std::uint16_t>(v);
    }
    u.ok = !u.host.empty();
    return u;
}

/// 从 SDP 里抠出所有 a=control: 的值（track 的相对 URL）。
std::vector<std::string> extract_controls(std::string_view sdp)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    while ((pos = sdp.find("a=control:", pos)) != std::string_view::npos) {
        pos += 10;
        const auto eol = sdp.find("\r\n", pos);
        const auto len = (eol == std::string_view::npos) ? sdp.size() - pos : eol - pos;
        if (len > 0) {
            out.emplace_back(sdp.substr(pos, len));
        }
        pos += len;
    }
    return out;
}

class Client final : public Linkable,
                     public RtspHandler,
                     public std::enable_shared_from_this<Client> {
public:
    explicit Client(Url url) : m_url(std::move(url))
    {
        if (m_url.path != "/") {
            m_session_url = "rtsp://" + m_url.host + ":" + std::to_string(m_url.port) + m_url.path;
        } else {
            m_session_url = "rtsp://" + m_url.host + ":" + std::to_string(m_url.port);
        }
    }

    // ---- netmech::Linkable ----

    std::shared_ptr<ConnHandler> on_linkup(Conn& c) override
    {
        std::error_code ec;
        auto self = shared_from_this();
        m_session = RtspSession::create(c.shared_from_this(), self, ec);
        if (ec) {
            std::fprintf(stderr, "[client] create 失败: %s\n", ec.message().c_str());
            m_done.store(true);
            return nullptr;
        }
        return m_session;
    }

    void on_linkfail(const Address&, std::error_code ec) override
    {
        std::fprintf(stderr, "[client] 连接失败: %s\n", ec.message().c_str());
        m_failed.store(true);
        m_done.store(true);
    }

    // ---- rtspmech::RtspHandler ----

    void on_ready(RtspSession& s) override
    {
        std::printf("[client] 已连接 %s，开始 OPTIONS\n", m_session_url.c_str());
        RequestBuilder b("OPTIONS", m_session_url);
        s.send_request(b);
    }

    void on_response(RtspSession& s, const RtspResponse& resp) override
    {
        std::printf("[client] <- %d %.*s (CSeq=%u)\n", resp.status,
                    static_cast<int>(resp.reason.size()), resp.reason.data(), resp.cseq());

        switch (m_step) {
        case Step::options:
            if (resp.status == 401) { report_auth(resp); return; }
            if (resp.status != 200) { fail("OPTIONS"); return; }
            m_step = Step::describe;
            {
                RequestBuilder b("DESCRIBE", m_session_url);
                b.header("Accept", "application/sdp");
                s.send_request(b);
            }
            break;

        case Step::describe:
            if (resp.status != 200) { fail("DESCRIBE"); return; }
            m_sdp.assign(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
            std::printf("[client]    SDP %zu 字节\n", m_sdp.size());
            m_tracks = extract_controls(m_sdp);
            if (m_tracks.empty()) {
                m_tracks.emplace_back("trackID=0");   // 设备不写 control 时兜底
            }
            std::printf("[client]    发现 %zu 个 track\n", m_tracks.size());
            m_track_index = 0;
            m_step = Step::setup;
            send_setup(s);
            break;

        case Step::setup: {
            if (resp.status != 200) { fail("SETUP"); return; }
            RtspTransport t;
            if (parse_transport(resp.header("Transport"), t)) {
                std::printf("[client]    track[%zu] %s → %s, ch=%d/%d",
                            m_track_index,
                            m_tracks[m_track_index].c_str(),
                            t.mode == TransportMode::udp ? "UDP" : "interleaved",
                            t.ch1, t.ch2);
                if (t.mode == TransportMode::udp) {
                    std::printf("，服务端端口 %u/%u", t.server_rtp_port, t.server_rtcp_port);
                }
                std::printf("\n");
            }
            if (m_session_id.empty()) {
                m_session_id.assign(resp.session().data(), resp.session().size());
            }
            ++m_track_index;
            if (m_track_index < m_tracks.size()) {
                send_setup(s);
            } else {
                m_step = Step::play;
                RequestBuilder b("PLAY", m_session_url);
                b.header("Session", m_session_id);
                b.header("Range", "npt=0.000-");
                s.send_request(b);
            }
            break;
        }

        case Step::play:
            if (resp.status != 200) { fail("PLAY"); return; }
            std::printf("[client] PLAY 成功，Session=%s，等待数据...\n", m_session_id.c_str());
            m_step = Step::running;
            break;

        case Step::running:
        case Step::done:
            break;
        }
    }

    void on_interleaved(RtspSession&, int ch, const std::uint8_t* d, std::size_t n) override
    {
        if (is_rtcp(d, n)) {
            m_rtcp_count.fetch_add(1);
        } else {
            m_rtp_count.fetch_add(1);
            m_last_channel.store(ch);
        }
    }

    void on_close(std::error_code ec) override
    {
        if (ec) {
            std::printf("[client] 连接断开: %s\n", ec.message().c_str());
        }
        m_done.store(true);
    }

    void request_stop() { m_done.store(true); }
    bool done() const { return m_done.load(); }
    bool failed() const { return m_failed.load(); }
    int  rtp_count() const { return m_rtp_count.load(); }
    int  rtcp_count() const { return m_rtcp_count.load(); }
    int  last_channel() const { return m_last_channel.load(); }
    bool playing() const { return m_step == Step::running; }

private:
    enum class Step { options, describe, setup, play, running, done };

    void send_setup(RtspSession& s)
    {
        const std::string& track = m_tracks[m_track_index];
        // control 可能是绝对 URL，也可能只是相对 id
        std::string url;
        if (track.rfind("rtsp://", 0) == 0) {
            url = track;
        } else {
            url = m_session_url;
            if (!url.empty() && url.back() != '/') { url.push_back('/'); }
            url += track;
        }
        RequestBuilder b("SETUP", url);
        b.header("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
        if (!m_session_id.empty()) {
            b.header("Session", m_session_id);
        }
        s.send_request(b);
    }

    void report_auth(const RtspResponse& resp)
    {
        std::printf("[client] 服务端要求认证：\n    %.*s\n",
                    static_cast<int>(resp.www_authenticate().size()),
                    resp.www_authenticate().data());
        std::printf("[client] （认证握手不在 rtspmech 职责内，需应用自己实现）\n");
        m_done.store(true);
    }

    void fail(const char* what)
    {
        std::fprintf(stderr, "[client] %s 失败（状态码 %d）\n", what, m_status);
        m_failed.store(true);
        m_done.store(true);
    }

    Url                      m_url;
    std::string              m_session_url;
    std::string              m_sdp;
    std::string              m_session_id;
    std::vector<std::string> m_tracks;
    std::size_t              m_track_index = 0;
    std::shared_ptr<RtspSession> m_session;

    Step               m_step = Step::options;
    int                m_status = 0;
    std::atomic<bool>  m_done{false};
    std::atomic<bool>  m_failed{false};
    std::atomic<int>   m_rtp_count{0};
    std::atomic<int>   m_rtcp_count{0};
    std::atomic<int>   m_last_channel{-1};
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rtsp://host[:port]/path> [--timeout=秒]\n", argv[0]);
        return 2;
    }
    const Url url = parse_url(argv[1]);
    if (!url.ok) {
        std::fprintf(stderr, "URL 不合法: %s\n", argv[1]);
        return 2;
    }

    int timeout_sec = 5;
    for (int i = 2; i < argc; ++i) {
        if (std::strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout_sec = std::atoi(argv[i] + 10);
        }
    }

    if (!netmech::initialize(2)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }

    auto mech   = Mechanism::create();
    auto client = std::make_shared<Client>(url);

    std::error_code ec = mech->connect(Address{url.host, url.port}, client);
    if (ec) {
        std::fprintf(stderr, "connect: %s\n", ec.message().c_str());
        netmech::shutdown();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeout_sec);
    while (!client->done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }

    const bool playing = client->playing();
    const int  rtp     = client->rtp_count();
    const int  rtcp    = client->rtcp_count();

    if (playing) {
        std::printf("\n[client] 收到 RTP %d 包 / RTCP %d 包（channel=%d）\n",
                    rtp, rtcp, client->last_channel());
    }

    mech->close_all();
    netmech::shutdown();

    if (client->failed()) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("%s\n", playing ? "PASS（PLAY 成功）" : "FAIL（超时）");
    return playing ? 0 : 1;
}
