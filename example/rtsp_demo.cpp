// =============================================================================
//  rtsp_demo —— 最小 RTSP 客户端 + 假服务端
// =============================================================================
//  一个进程内起一对 TCP：
//      客户端                          假服务端
//        │  OPTIONS  ──────────────────►│
//        │  ◄──────────────  200 OK     │
//        │  DESCRIBE ──────────────────►│
//        │  ◄──────────────  200 + SDP  │
//        │  SETUP    ──────────────────►│
//        │  ◄──  200 + Transport(端口)   │
//        │  PLAY     ──────────────────►│
//        │  ◄──────────────  200 OK     │
//
//  演示的是 rtspmech 的用法，不涉及真实的媒体收发——那是应用的事。
// =============================================================================
#include <rtspmech/rtspmech.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace rtspmech;
using namespace netmech;

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kSdp =
    "v=0\r\n"
    "o=- 1234567890 1 IN IP4 127.0.0.1\r\n"
    "s=Demo Stream\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=control:trackID=1\r\n"
    "m=audio 0 RTP/AVP 97\r\n"
    "a=rtpmap:97 MPEG4-GENERIC/16000/1\r\n"
    "a=control:trackID=2\r\n";

// =============================================================================
//  假服务端
// =============================================================================

class FakeServer final : public Linkable,
                         public RtspHandler,
                         public std::enable_shared_from_this<FakeServer> {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn& c) override
    {
        std::error_code ec;
        auto self = shared_from_this();
        auto s = RtspSession::create(c.shared_from_this(), self, ec);
        if (ec) {
            std::fprintf(stderr, "[server] create: %s\n", ec.message().c_str());
        }
        return s;
    }

    void on_request(RtspSession& sess, const RtspRequest& req) override
    {
        std::printf("[server] <- %.*s %.*s\n",
                    static_cast<int>(req.method.size()), req.method.data(),
                    static_cast<int>(req.uri.size()), req.uri.data());

        if (req.method == "OPTIONS") {
            auto wire = ResponseBuilder(200, "OK")
                            .cseq(req.cseq())
                            .header("Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN")
                            .build();
            sess.send_response(std::move(wire));
        } else if (req.method == "DESCRIBE") {
            auto wire = ResponseBuilder(200, "OK")
                            .cseq(req.cseq())
                            .header("Content-Type", "application/sdp")
                            .body(kSdp)
                            .build();
            sess.send_response(std::move(wire));
        } else if (req.method == "SETUP") {
            // 回一个 UDP 单播的 Transport，告诉客户端我从哪个端口发
            auto wire = ResponseBuilder(200, "OK")
                            .cseq(req.cseq())
                            .header("Transport",
                                    "RTP/AVP;unicast;client_port=5000-5001;"
                                    "server_port=6000-6001;ssrc=0BADCAFE")
                            .header("Session", "12345678;timeout=60")
                            .build();
            sess.send_response(std::move(wire));
        } else if (req.method == "PLAY") {
            auto wire = ResponseBuilder(200, "OK")
                            .cseq(req.cseq())
                            .header("Session", "12345678")
                            .header("Range", "npt=0.000-")
                            .build();
            sess.send_response(std::move(wire));
        } else {
            auto wire = ResponseBuilder(501, "Not Implemented")
                            .cseq(req.cseq())
                            .build();
            sess.send_response(std::move(wire));
        }
    }
};

// =============================================================================
//  客户端
// =============================================================================

class DemoClient final : public Linkable,
                         public RtspHandler,
                         public std::enable_shared_from_this<DemoClient> {
public:
    explicit DemoClient(std::string url) : m_url(std::move(url)) {}

    std::shared_ptr<ConnHandler> on_linkup(Conn& c) override
    {
        std::error_code ec;
        auto self = shared_from_this();
        m_session = RtspSession::create(c.shared_from_this(), self, ec);
        if (ec) {
            std::fprintf(stderr, "[client] create: %s\n", ec.message().c_str());
            return nullptr;
        }
        return m_session;
    }

    /// 连接就绪：在这里发第一个请求。
    void on_ready(RtspSession& sess) override
    {
        std::printf("[client] 连接就绪，开始 OPTIONS\n");
        RequestBuilder b("OPTIONS", m_url);
        sess.send_request(b);
    }

    void on_response(RtspSession& sess, const RtspResponse& resp) override
    {
        std::printf("[client] -> %d %.*s (CSeq=%u)\n",
                    resp.status,
                    static_cast<int>(resp.reason.size()), resp.reason.data(),
                    resp.cseq());

        switch (m_step) {
        case Step::options:
            if (resp.status != 200) { fail("OPTIONS"); return; }
            m_step = Step::describe;
            {
                RequestBuilder b("DESCRIBE", m_url);
                b.header("Accept", "application/sdp");
                sess.send_request(b);
            }
            break;

        case Step::describe:
            if (resp.status != 200) { fail("DESCRIBE"); return; }
            // SDP 是借用视图，要留下来处理就必须拷贝
            m_sdp.assign(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
            std::printf("[client]    拿到 SDP %zu 字节\n", m_sdp.size());
            m_step = Step::setup;
            {
                RequestBuilder b("SETUP", m_url + "/trackID=1");
                b.header("Transport", "RTP/AVP;unicast;client_port=5000-5001");
                sess.send_request(b);
            }
            break;

        case Step::setup:
            if (resp.status != 200) { fail("SETUP"); return; }
            m_session_id.assign(resp.session().data(), resp.session().size());
            {
                RtspTransport t;
                if (parse_transport(resp.header("Transport"), t)
                    && t.mode == TransportMode::udp) {
                    std::printf("[client]    服务端从 %u/%u 发流，ssrc=%08X\n",
                                t.server_rtp_port, t.server_rtcp_port, t.ssrc);
                }
                std::printf("[client]    Session=%s\n", m_session_id.c_str());
            }
            m_step = Step::play;
            {
                RequestBuilder b("PLAY", m_url);
                b.header("Session", m_session_id);
                b.header("Range", "npt=0.000-");
                sess.send_request(b);
            }
            break;

        case Step::play:
            if (resp.status != 200) { fail("PLAY"); return; }
            std::printf("[client] 流程完成，可以开始收流了\n");
            m_done.store(true);
            m_step = Step::done;
            sess.close();
            break;

        case Step::done:
            break;
        }
    }

    bool done() const { return m_done.load(); }

private:
    enum class Step { options, describe, setup, play, done };

    void fail(const char* what)
    {
        std::fprintf(stderr, "[client] %s 失败\n", what);
        m_done.store(true);
        m_step = Step::done;
    }

    std::string           m_url;
    std::string           m_sdp;
    std::string           m_session_id;
    std::shared_ptr<RtspSession> m_session;
    Step                  m_step = Step::options;
    std::atomic<bool>     m_done{false};
};

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    if (!netmech::initialize(2)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }

    const auto port = find_free_port_run(30000, 40000, 1, 1);
    if (!port) {
        std::fprintf(stderr, "找不到可用端口\n");
        return 1;
    }
    const Address addr{"127.0.0.1", *port};

    auto mech   = Mechanism::create();
    auto server = std::make_shared<FakeServer>();
    auto client = std::make_shared<DemoClient>("rtsp://127.0.0.1/demo");

    std::error_code ec;
    mech->set_linkable(server);
    if ((ec = mech->listen(addr, server))) {
        std::fprintf(stderr, "listen: %s\n", ec.message().c_str());
        return 1;
    }
    if ((ec = mech->connect(addr, client))) {
        std::fprintf(stderr, "connect: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("demo 启动，监听 %s\n\n", addr.to_string().c_str());

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!client->done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }

    mech->close_all();
    netmech::shutdown();

    const bool ok = client->done();
    std::printf("\n%s\n", ok ? "PASS" : "FAIL（超时）");
    return ok ? 0 : 1;
}
