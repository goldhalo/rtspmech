// ============================================================================
//  test_server_connector.cpp —— 只用**公共 API** 跑通一次完整对话
//
//  这个用例刻意**不包含任何 netmech 头**：它就是"公共头不依赖 netmech"的
//  可执行证据 —— 别人拿 lib + include 就能写出一个能跑的 RTSP 服务端和客户端。
//
//  验证：
//    · RtspServer 自己监听、接连接、造会话
//    · RtspConnector 自己连出去、造会话
//    · 客户端 on_ready 发 OPTIONS，服务端 on_request 回 200，客户端 on_response 收到
// ============================================================================
#include <rtspmech/rtspmech.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <functional>
#include <system_error>
#include <utility>

#include "test_framework.hpp"

using namespace rtspmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

/// 服务端：任何请求都回 200，并把方法名记下来。
class EchoHandler final : public RtspHandler {
public:
    void on_request(RtspSession& sess, const RtspRequest& req) override
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            last_method.assign(req.method.data(), req.method.size());
        }
        requests.fetch_add(1);

        std::string wire = ResponseBuilder(200, "OK").cseq(req.cseq()).build();
        sess.send_response(std::move(wire));
    }

    /// 加锁读（io 线程写、主线程读）
    std::string method() const
    {
        std::lock_guard<std::mutex> lk(mu);
        return last_method;
    }

    std::atomic<int>  requests{0};
    mutable std::mutex mu;
    std::string        last_method;
};

class EchoAcceptor final : public RtspAcceptor {
public:
    std::shared_ptr<RtspHandler> on_accept(const Endpoint&) override
    {
        auto h = std::make_shared<EchoHandler>();
        {
            std::lock_guard<std::mutex> lk(mu);
            last = h;
        }
        return h;
    }

    /// 加锁读（io 线程写、主线程读）
    std::shared_ptr<EchoHandler> get_last() const
    {
        std::lock_guard<std::mutex> lk(mu);
        return last;
    }

    mutable std::mutex           mu;
    std::shared_ptr<EchoHandler> last;
};

/// 客户端：连上就发 OPTIONS，记录响应状态。
class ConnectorTestHandler final : public RtspHandler {
public:
    void on_ready(RtspSession& sess) override
    {
        session     = sess.shared_from_this();
        ready.store(true);
        RequestBuilder b("OPTIONS", "rtsp://127.0.0.1/");
        sess.send_request(b);
    }

    void on_response(RtspSession&, const RtspResponse& resp) override
    {
        last_status.store(resp.status);
        responses.fetch_add(1);
    }

    void on_link_failed(std::error_code ec) override
    {
        link_error.store(ec.value());
    }

    void on_close(RtspSession&, std::error_code) override { closed.store(true); }

    std::shared_ptr<RtspSession> session;
    std::atomic<bool>            ready{false};
    std::atomic<bool>            closed{false};
    std::atomic<int>             responses{0};
    std::atomic<int>             last_status{0};
    std::atomic<int>             link_error{0};
};

} // namespace

NETMECH_TEST(public_api_server_and_connector_roundtrip)
{
    // ---- 服务端：端口交给系统分配 ----
    auto acceptor = std::make_shared<EchoAcceptor>();

    RtspServerConfig scfg;
    scfg.local = {"127.0.0.1", 0};

    auto server = RtspServer::create(scfg, acceptor);
    REQUIRE(server != nullptr);
    REQUIRE(!server->start());
    REQUIRE(server->running());

    const Endpoint bound = server->local();
    REQUIRE(bound.port() != 0);

    // ---- 客户端：连上去 ----
    auto ch = std::make_shared<ConnectorTestHandler>();

    RtspConnectorConfig ccfg;
    ccfg.remote = bound;

    auto conn = RtspConnector::create(ccfg, ch);
    REQUIRE(conn != nullptr);
    REQUIRE(!conn->connect());

    // 1) 会话就绪（客户端）
    REQUIRE(wait_until([&] { return ch->ready.load(); }, 3000ms));
    CHECK_EQ(ch->link_error.load(), 0);

    // 2) 服务端收到请求
    REQUIRE(wait_until(
        [&] {
            const auto h = acceptor->get_last();
            return h && h->requests.load() == 1;
        },
        3000ms));
    CHECK(acceptor->get_last()->method() == "OPTIONS");

    // 3) 客户端收到 200
    REQUIRE(wait_until([&] { return ch->responses.load() == 1; }, 3000ms));
    CHECK_EQ(ch->last_status.load(), 200);

    // 4) 关掉，两端都要收到 on_close
    conn->close();
    REQUIRE(wait_until([&] { return ch->closed.load(); }, 3000ms));

    server->stop();
    CHECK(!server->running());

    conn.reset();
    server.reset();
}

// ---------------------------------------------------------------------------
// connect 期间 close()：之后即使 TCP 已连上，也不能再冒出一个会话
//
//   以前 on_linkup 不检查"是否已关闭"，照样 make_conn_adapter → 造会话 →
//   回调 on_ready。应用已经放弃连接，却凭空收到一个会话。
//
//   判定用不变量：**不允许出现"ready=true 且一直不 closed"**。
//   （linkup 若在 close 之前赢得竞争，会话是合法的，close 会补上 on_close。）
// ---------------------------------------------------------------------------
NETMECH_TEST(connector_close_during_connect_creates_no_session)
{
    auto acceptor = std::make_shared<EchoAcceptor>();

    RtspServerConfig scfg;
    scfg.local = {"127.0.0.1", 0};
    auto server = RtspServer::create(scfg, acceptor);
    REQUIRE(server != nullptr);
    REQUIRE(!server->start());

    auto ch = std::make_shared<ConnectorTestHandler>();
    RtspConnectorConfig ccfg;
    ccfg.remote = server->local();

    auto conn = RtspConnector::create(ccfg, ch);
    REQUIRE(conn != nullptr);
    REQUIRE(!conn->connect());
    conn->close();   // 立刻取消（此时 conn 可能还没建立）

    // 给晚到的 linkup 足够时间：若它还是造了会话，ready 会变 true 而没有 on_close
    REQUIRE(wait_until([&] {
        return ch->ready.load() && ch->closed.load();
    }, 500ms) || (!ch->ready.load() && !ch->closed.load()));

    CHECK_MSG(!(ch->ready.load() && !ch->closed.load()),
              "close 之后不应再出现活着的会话（late linkup 未被拒绝）");
    CHECK(!conn->connected());

    server->stop();
}
