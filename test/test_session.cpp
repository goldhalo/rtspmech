// ============================================================================
//  test_session.cpp —— RtspSession 端到端测试
//
//  真起一对 TCP（netmech 的 Mechanism），一边服务端一边客户端，走完整链路：
//    客户端发 OPTIONS → 服务端收到 → 回 200 → 客户端收到响应
//  再验 interleaved（$ 帧）双向。
// ============================================================================
#include <rtspmech/rtspmech.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "test_framework.hpp"

using namespace rtspmech;
using namespace netmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/// 服务端：收到任何请求都回 200，并把收到的 interleaved 帧原样回发。
class TestServer : public Linkable, public RtspHandler,
                   public std::enable_shared_from_this<TestServer> {
public:
    std::shared_ptr<ConnHandler> on_accept(Conn& c) override
    {
        std::error_code ec;
        auto self = shared_from_this();
        std::shared_ptr<RtspHandler> self_h(self, static_cast<RtspHandler*>(this));
        std::weak_ptr<RtspHandler> wh = self_h;
        auto s = RtspSession::create(c.shared_from_this(), wh, ec);
        if (!ec) {
            m_last_session = s;
        }
        return s;
    }

    void on_request(RtspSession& s, const RtspRequest& req) override
    {
        requests.fetch_add(1);
        last_method.assign(req.method.data(), req.method.size());

        std::string wire = ResponseBuilder(200, "OK").cseq(req.cseq()).build();
        s.send_response(std::move(wire));
    }

    void on_interleaved(RtspSession& s, int ch, const std::uint8_t* d, std::size_t n) override
    {
        got_binary.fetch_add(1);
        last_channel.store(ch);
        s.send_interleaved(ch, d, n);      // 原样回发
    }

    std::atomic<int>  requests{0};
    std::atomic<int>  got_binary{0};
    std::atomic<int>  last_channel{-1};
    std::string       last_method;
    std::shared_ptr<RtspSession> m_last_session;
};

/// 客户端：连上就发 OPTIONS。
class TestClient : public Linkable, public RtspHandler,
                   public std::enable_shared_from_this<TestClient> {
public:
    std::shared_ptr<ConnHandler> on_linkup(Conn& c) override
    {
        std::error_code ec;
        auto self = shared_from_this();
        std::shared_ptr<RtspHandler> self_h(self, static_cast<RtspHandler*>(this));
        std::weak_ptr<RtspHandler> wh = self_h;
        m_session = RtspSession::create(c.shared_from_this(), wh, ec);
        if (ec) {
            link_error = ec;
            return nullptr;
        }
        return m_session;
    }

    void on_ready(RtspSession& s) override
    {
        // ★ 这里才发第一个请求：on_linkup 阶段连接还没被 netmech 驱动
        RequestBuilder b("OPTIONS", "rtsp://127.0.0.1/");
        s.send_request(b);
    }

    void on_response(RtspSession&, const RtspResponse& resp) override
    {
        last_status.store(resp.status);
        got_response.fetch_add(1);
    }

    void on_interleaved(RtspSession&, int ch, const std::uint8_t*, std::size_t n) override
    {
        got_binary.fetch_add(1);
        last_channel.store(ch);
        last_len.store(n);
    }

    std::shared_ptr<RtspSession> m_session;
    std::error_code              link_error;
    std::atomic<int>             got_response{0};
    std::atomic<int>             last_status{0};
    std::atomic<int>             got_binary{0};
    std::atomic<int>             last_channel{-1};
    std::atomic<std::size_t>     last_len{0};
};

struct Fixture {
    std::shared_ptr<Mechanism> mech;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TestClient> client;
    std::error_code             err;     ///< 建链失败原因
};

/// 起一对 服务端/客户端，监听在回环的随机端口。
Fixture start_pair()
{
    Fixture f;
    f.mech   = Mechanism::create();
    f.server = std::make_shared<TestServer>();
    f.client = std::make_shared<TestClient>();

    std::error_code ec;
    // listen 需要用固定端口：先探一个
    auto port = netmech::find_free_port_run(30000, 40000, 1, 1);
    if (!port) {
        return f;
    }
    const Address addr{"127.0.0.1", *port};

    f.mech->set_linkable(f.server);
    ec = f.mech->listen(addr, f.server);
    if (ec) {
        f.err = ec;
        return f;
    }

    ec = f.mech->connect(addr, f.client);
    if (ec) {
        f.err = ec;
        return f;
    }
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 客户端发 OPTIONS，服务端回 200
// ---------------------------------------------------------------------------
NETMECH_TEST(session_request_response_roundtrip)
{
    auto f = start_pair();
    REQUIRE(f.mech != nullptr);
    CHECK_MSG(!f.err, ("建链失败: " + f.err.message()).c_str());
    REQUIRE(!f.err);

    CHECK(wait_for([&] { return f.client->got_response.load() > 0; }, 5000ms));
    CHECK_EQ(f.client->last_status.load(), 200);
    CHECK(f.server->requests.load() >= 1);
    CHECK(f.server->last_method == "OPTIONS");

    f.mech->close_all();
}

// ---------------------------------------------------------------------------
// 2. interleaved：客户端发 $ 帧，服务端收到并原样回发
// ---------------------------------------------------------------------------
NETMECH_TEST(session_interleaved_roundtrip)
{
    auto f = start_pair();
    REQUIRE(f.mech != nullptr);
    REQUIRE(wait_for([&] { return f.client->got_response.load() > 0; }, 5000ms));

    // 等客户端拿到 session
    std::this_thread::sleep_for(50ms);
    REQUIRE(f.client->m_session != nullptr);
    REQUIRE(f.client->m_session->is_open());

    const std::uint8_t payload[16] = {
        0x80, 0x60, 0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    };
    CHECK(f.client->m_session->send_interleaved(0, payload, sizeof(payload)));

    // 服务端收到 → 回发 → 客户端收到
    CHECK(wait_for([&] { return f.client->got_binary.load() > 0; }, 5000ms));
    CHECK(f.server->got_binary.load() >= 1);
    CHECK_EQ(f.server->last_channel.load(), 0);
    CHECK_EQ(f.client->last_channel.load(), 0);
    CHECK_EQ(f.client->last_len.load(), sizeof(payload));

    f.mech->close_all();
}
