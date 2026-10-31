// =============================================================================
//  client.cpp —— 完整 RTSP 客户端实现
// =============================================================================
//  状态机：OPTIONS → DESCRIBE → SETUP(每个 track) → PLAY
//
//  一个 RtspClient 实例对应一条 TCP 连接 + 一个 RtspSession。
//  媒体通道（UDP 端口 / interleaved 通道）只协商，不在这里收发。
// =============================================================================
#include <rtspmech/client.hpp>
#include <rtspmech/common.hpp>
#include <rtspmech/rtsp_transport.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace rtspmech {

namespace {

/// 从 SDP 里抠 `a=control:` 的值。
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

/// 拆 `rtsp://host[:port]/path`。
struct ParsedUrl {
    std::string   host;
    std::uint16_t port = 554;
    std::string   path;
    bool          ok = false;
};

ParsedUrl parse_url(std::string_view s)
{
    ParsedUrl u;
    constexpr std::string_view prefix = "rtsp://";
    if (s.size() <= prefix.size() || s.substr(0, prefix.size()) != prefix) {
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
        unsigned v = 0;
        for (char c : hostport.substr(colon + 1)) {
            if (c < '0' || c > '9') { return u; }
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (v == 0 || v > 65535) { return u; }
        u.port = static_cast<std::uint16_t>(v);
    }
    u.ok = !u.host.empty();
    return u;
}

} // namespace

// =============================================================================
//  Impl
// =============================================================================

class RtspClient::Impl : public netmech::Linkable,
                         public RtspHandler,
                         public std::enable_shared_from_this<RtspClient::Impl> {
public:
    Config                            cfg;
    std::shared_ptr<ClientHandler>    handler;

    std::shared_ptr<netmech::Mechanism> mech;
    std::shared_ptr<RtspSession>        session;

    std::atomic<bool>  playing{false};
    std::atomic<bool>  failed{false};
    std::string        session_id;
    std::string        sdp;
    std::uint16_t      server_rtp_port = 0;
    std::uint16_t      server_rtcp_port = 0;
    std::uint16_t      client_rtp_port = 0;
    std::atomic<bool>  recording{false};

    ParsedUrl          url;
    std::vector<std::string> tracks;
    std::size_t        track_index = 0;
    int                last_status = 0;

    enum class Step { options, describe, announce, setup, play, record, done };
    Step step = Step::options;

    // ---- netmech::Linkable ----

    std::shared_ptr<netmech::ConnHandler> on_linkup(netmech::Conn& c) override
    {
        std::error_code ec;
        session = RtspSession::create(c.shared_from_this(), weak_of_this(), ec);
        if (ec) {
            fail("connect", ec, 0);
            return nullptr;
        }
        return session;
    }

    void on_linkfail(const netmech::Address&, std::error_code ec) override
    {
        fail("connect", ec, 0);
    }

    // ---- rtspmech::RtspHandler ----

    void on_ready(RtspSession& s) override
    {
        // 推流模式没有 DESCRIBE，track 列表从自己的 SDP 里取（或默认一个）
        if (cfg.mode == ClientMode::push) {
            tracks = extract_controls(cfg.sdp);
            if (tracks.empty()) {
                tracks.emplace_back("trackID=0");
            }
        }
        RequestBuilder b("OPTIONS", cfg.url);
        s.send_request(b);
    }

    void on_response(RtspSession& s, const RtspResponse& resp) override
    {
        last_status = resp.status;

        // 401：交给应用
        if (resp.status == 401) {
            if (handler) {
                handler->on_unauthorized(
                    facade(),
                    std::string_view(resp.www_authenticate().data(),
                                     resp.www_authenticate().size()));
            }
            return;
        }

        switch (step) {
        case Step::options:
            if (resp.status != 200) { fail("OPTIONS", std::error_code{}, resp.status); return; }
            if (cfg.mode == ClientMode::push) {
                // 推流：ANNOUNCE 带 SDP
                step = Step::announce;
                RequestBuilder b("ANNOUNCE", cfg.url);
                b.header("Content-Type", "application/sdp");
                b.body(cfg.sdp);
                s.send_request(b);
            } else {
                // 拉流：DESCRIBE
                step = Step::describe;
                RequestBuilder b("DESCRIBE", cfg.url);
                b.header("Accept", "application/sdp");
                s.send_request(b);
            }
            break;

        case Step::announce:
            if (resp.status != 200) { fail("ANNOUNCE", std::error_code{}, resp.status); return; }
            step = Step::setup;
            track_index = 0;
            send_setup(s);
            break;

        case Step::describe:
            if (resp.status != 200) { fail("DESCRIBE", std::error_code{}, resp.status); return; }
            sdp.assign(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
            tracks = extract_controls(sdp);
            if (tracks.empty()) {
                tracks.emplace_back("trackID=0");
            }
            track_index = 0;
            step = Step::setup;
            send_setup(s);
            break;

        case Step::setup: {
            if (resp.status != 200) { fail("SETUP", std::error_code{}, resp.status); return; }

            RtspTransport t;
            if (parse_transport(resp.header("Transport"), t)) {
                if (t.mode == TransportMode::udp) {
                    server_rtp_port  = t.server_rtp_port;
                    server_rtcp_port = t.server_rtcp_port;
                    if (client_rtp_port == 0) {
                        client_rtp_port = t.client_rtp_port;
                    }
                }
            }
            if (session_id.empty()) {
                session_id.assign(resp.session().data(), resp.session().size());
            }

            ++track_index;
            if (track_index < tracks.size()) {
                send_setup(s);
            } else if (cfg.mode == ClientMode::push) {
                // 推流：RECORD
                step = Step::record;
                RequestBuilder b("RECORD", cfg.url);
                b.header("Session", session_id);
                b.header("Range", "npt=0.000-");
                s.send_request(b);
            } else {
                // 拉流：PLAY
                step = Step::play;
                RequestBuilder b("PLAY", cfg.url);
                b.header("Session", session_id);
                b.header("Range", "npt=0.000-");
                s.send_request(b);
            }
            break;
        }

        case Step::record:
            if (resp.status != 200) { fail("RECORD", std::error_code{}, resp.status); return; }
            recording.store(true);
            step = Step::done;
            if (handler) { handler->on_recording(facade()); }
            break;

        case Step::play:
            if (resp.status != 200) { fail("PLAY", std::error_code{}, resp.status); return; }
            step = Step::play;
            playing.store(true);
            step = Step::done;
            if (handler) { handler->on_playing(facade()); }
            break;

        case Step::done:
            break;
        }
    }

    void on_interleaved(RtspSession&, int, const std::uint8_t*, std::size_t) override
    {
        // 媒体由应用处理（这个类只管信令）
    }

    void on_close(RtspSession&, std::error_code ec) override
    {
        playing.store(false);
        if (handler) {
            (void)ec;
            handler->on_closed(facade());
        }
    }

    // ---- 内部 ----

    void send_setup(RtspSession& s)
    {
        const std::string& track = tracks[track_index];
        std::string target;
        if (track.rfind("rtsp://", 0) == 0) {
            target = track;
        } else {
            target = cfg.url;
            if (!target.empty() && target.back() != '/') { target.push_back('/'); }
            target += track;
        }

        RequestBuilder b("SETUP", target);
        if (cfg.transport == TransportPreference::udp) {
            // 应用自己决定端口；0 表示让服务端在响应里告诉我们用了哪个
            std::string tr = "RTP/AVP;unicast";
            if (cfg.client_rtp_port != 0) {
                tr += ";client_port=" + std::to_string(cfg.client_rtp_port) + "-" +
                      std::to_string(cfg.client_rtp_port + 1);
            }
            b.header("Transport", tr);
        } else {
            b.header("Transport",
                     "RTP/AVP/TCP;unicast;interleaved=" +
                         std::to_string(cfg.channel) + "-" +
                         std::to_string(cfg.channel + 1));
        }
        if (!session_id.empty()) {
            b.header("Session", session_id);
        }
        s.send_request(b);
    }

    void fail(const char* stage, std::error_code ec, int status)
    {
        if (failed.exchange(true)) {
            return;
        }
        playing.store(false);
        if (handler) {
            handler->on_failed(facade(), stage, ec, status);
        }
    }

    RtspClient& facade();
    std::weak_ptr<RtspHandler> weak_of_this();

    RtspClient* owner = nullptr;
};

// Impl 需要拿到外层的 RtspClient&（回调里要给应用）
RtspClient& RtspClient::Impl::facade()
{
    return *owner;
}

std::weak_ptr<RtspHandler> RtspClient::Impl::weak_of_this()
{
    // Impl 同时是 RtspHandler，用 aliasing 构造出 weak_ptr<RtspHandler>
    auto self = shared_from_this();
    std::shared_ptr<RtspHandler> h(self, static_cast<RtspHandler*>(this));
    return h;
}

// =============================================================================
//  RtspClient
// =============================================================================

RtspClient::RtspClient() = default;

RtspClient::~RtspClient()
{
    if (m_impl && m_impl->mech) {
        m_impl->mech->close_all();
    }
}

std::shared_ptr<RtspClient> RtspClient::create(const Config& cfg,
                                               std::shared_ptr<ClientHandler> handler,
                                               std::error_code& ec)
{
    ec.clear();

    const ParsedUrl u = parse_url(cfg.url);
    if (!u.ok) {
        ec = make_error_code(RtspErrc::invalid_message);
        return nullptr;
    }

    auto client = std::shared_ptr<RtspClient>(new RtspClient());
    auto impl = std::make_shared<Impl>();
    impl->cfg     = cfg;
    impl->handler = std::move(handler);
    impl->url     = u;
    impl->owner   = client.get();

    impl->mech = netmech::Mechanism::create();
    if (!impl->mech) {
        ec = make_error_code(RtspErrc::invalid_message);
        return nullptr;
    }

    impl->mech->set_linkable(impl);
    ec = impl->mech->connect(netmech::Address{u.host, u.port}, impl);
    if (ec) {
        return nullptr;
    }

    client->m_impl = impl;
    return client;
}

bool RtspClient::is_playing() const noexcept
{
    return m_impl && m_impl->playing.load();
}

const std::string& RtspClient::session_id() const noexcept
{
    static const std::string kEmpty;
    return m_impl ? m_impl->session_id : kEmpty;
}

const std::string& RtspClient::sdp() const noexcept
{
    static const std::string kEmpty;
    return m_impl ? m_impl->sdp : kEmpty;
}

std::uint16_t RtspClient::server_rtp_port() const noexcept
{
    return m_impl ? m_impl->server_rtp_port : 0;
}

std::uint16_t RtspClient::server_udp_port() const noexcept
{
    return m_impl ? m_impl->server_rtp_port : 0;
}

std::uint16_t RtspClient::server_rtcp_port() const noexcept
{
    return m_impl ? m_impl->server_rtcp_port : 0;
}

int RtspClient::channel() const noexcept
{
    return m_impl ? m_impl->cfg.channel : 0;
}

bool RtspClient::is_recording() const noexcept
{
    return m_impl && m_impl->recording.load();
}

ClientMode RtspClient::mode() const noexcept
{
    return m_impl ? m_impl->cfg.mode : ClientMode::pull;
}

RtspSession* RtspClient::session() noexcept
{
    return m_impl ? m_impl->session.get() : nullptr;
}

void RtspClient::teardown()
{
    if (!m_impl || !m_impl->session || !m_impl->playing.load()) {
        return;
    }
    RequestBuilder b("TEARDOWN", m_impl->cfg.url);
    if (!m_impl->session_id.empty()) {
        b.header("Session", m_impl->session_id);
    }
    m_impl->session->send_request(b);
    m_impl->playing.store(false);
    m_impl->session->close();
}

} // namespace rtspmech
