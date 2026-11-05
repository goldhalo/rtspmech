// ============================================================================
//  test_client_flow.cpp —— RtspClient 的**收尾链路**（以前零覆盖）
//
//  为什么单开一个文件：`RtspClient` 在测试集里**一条用例都没有**，全靠 example
//  的手工脚本 soak.sh。于是下面四个问题全部"测试全绿"：
//    · 推流模式 teardown() 静默失效（🔴）
//    · 401 之后状态机永久卡死、没有重放入口（🟠）
//    · on_close 的 error_code 永远是空（🟠）
//    · 服务端连接表只进不出（🟠）
//
//  这里全部用**公共 API**（RtspServer + RtspClient），不碰内部头。
// ============================================================================
#include <rtspmech/rtspmech.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <system_error>

#include "test_framework.hpp"

using namespace rtspmech;
using namespace netmech_test;

namespace {

using namespace std::chrono_literals;

bool wait_until(const std::function<bool()>& pred,
                std::chrono::milliseconds       timeout = 3000ms)
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

/// 服务端桩：按方法名应答；可配置成"第一次 OPTIONS 要认证"。
class ServerStub final : public RtspHandler {
public:
    void on_request(RtspSession& sess, const RtspRequest& req) override
    {
        const std::string method(req.method.data(), req.method.size());
        {
            std::lock_guard<std::mutex> lk(mu);
            methods.push_back(method);
            if (method == "TEARDOWN") {
                teardown_seen.store(true);
            }
        }

        // 认证闸门：只拦 OPTIONS（够验证 401 路径，又不影响后续步骤）
        if (require_auth && method == "OPTIONS") {
            if (req.header("Authorization").empty()) {
                ResponseBuilder b(401, "Unauthorized");
                b.header("WWW-Authenticate", "Basic realm=\"ut\"");
                b.cseq(req.cseq());
                sess.send_response(b.build());
                return;
            }
            auth_seen.store(true);
        }

        ResponseBuilder b(200, "OK");
        b.cseq(req.cseq());
        if (method == "SETUP") {
            b.header("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
        }
        if (method == "DESCRIBE") {
            b.header("Content-Type", "application/sdp");
            b.body("v=0\r\n"
                   "m=video 0 RTP/AVP 96\r\n"
                   "a=rtpmap:96 H264/90000\r\n"
                   "a=control:trackID=0\r\n");
        }
        if (method != "OPTIONS" && method != "ANNOUNCE") {
            b.header("Session", "UT-SESSION;timeout=60");
        }
        sess.send_response(b.build());
    }

    /// 会话一就绪就吐一段**非法报文**给对端（用来造"异常断链"）。
    void on_ready(RtspSession& sess) override
    {
        if (send_garbage) {
            sess.send_response("THIS IS NOT AN RTSP MESSAGE\r\n\r\n");
        }
    }

    bool saw(const char* m)
    {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& x : methods) {
            if (x == m) {
                return true;
            }
        }
        return false;
    }

    std::mutex               mu;
    std::vector<std::string> methods;
    std::atomic<bool>        teardown_seen{false};
    std::atomic<bool>        auth_seen{false};
    bool                     require_auth = false;
    bool                     send_garbage = false;
};

class ServerAcceptor final : public RtspAcceptor {
public:
    explicit ServerAcceptor(std::shared_ptr<ServerStub> h) : m_h(std::move(h)) {}
    std::shared_ptr<RtspHandler> on_accept(const Endpoint&) override { return m_h; }
    std::shared_ptr<ServerStub>  m_h;
};

/// 客户端桩：记录各阶段结果。
class ClientStub final : public ClientHandler {
public:
    void on_playing(RtspClient&) override { playing.store(true); }
    void on_recording(RtspClient&) override { recording.store(true); }

    void on_failed(RtspClient&, const char* stage, std::error_code ec,
                   int status) override
    {
        if (stage != nullptr) {
            fail_stage.assign(stage);
        }
        fail_ec.store(ec.value());
        fail_status.store(status);
        failed.store(true);
    }

    void on_unauthorized(RtspClient& c, std::string_view) override
    {
        unauthorized.fetch_add(1);
        if (auto_retry) {
            (void)c.retry_with_credentials("user", "pass");
        } else if (auto_abort) {
            c.abort_unauthorized("OPTIONS");
        }
    }

    void on_closed(RtspClient&, std::error_code ec) override
    {
        close_ec.store(ec.value());
        closed.store(true);
    }

    std::atomic<bool> playing{false};
    std::atomic<bool> recording{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> closed{false};
    std::atomic<int>  unauthorized{0};
    std::atomic<int>  fail_ec{0};
    std::atomic<int>  fail_status{0};
    std::atomic<int>  close_ec{0};
    std::string       fail_stage;

    bool auto_retry = false;
    bool auto_abort = false;
};

struct Rig {
    std::shared_ptr<ServerStub>  stub;
    std::shared_ptr<RtspServer>  server;
    std::uint16_t                port = 0;

    bool start()
    {
        stub = std::make_shared<ServerStub>();
        RtspServerConfig scfg;
        scfg.local = {"127.0.0.1", 0};
        server     = RtspServer::create(scfg, std::make_shared<ServerAcceptor>(stub));
        if (!server || server->start()) {
            return false;
        }
        port = server->local().port();
        return port != 0;
    }
};

std::string url_for(std::uint16_t port)
{
    return "rtsp://127.0.0.1:" + std::to_string(port) + "/live";
}

const char kPushSdp[] =
    "v=0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n";

} // namespace

// ---------------------------------------------------------------------------
// 1. 推流：RECORD 成功之后调 teardown()，服务端**必须**收到 TEARDOWN
//
//    这不是"功能增强"，是修一个静默失效：teardown 的入口条件写的是
//    `playing`，而推流走的是 recording 分支，playing 永远是 false ——
//    于是整个调用什么都不做、也不报错。报告把它列为 🔴 是准确的。
// ---------------------------------------------------------------------------
NETMECH_TEST(client_push_teardown_reaches_server)
{
    Rig rig;
    REQUIRE(rig.start());

    RtspClient::Config ccfg;
    ccfg.mode      = ClientMode::push;
    ccfg.url       = url_for(rig.port);
    ccfg.sdp       = kPushSdp;
    ccfg.transport = TransportPreference::interleaved;

    auto handler = std::make_shared<ClientStub>();
    std::error_code ec;
    auto client = RtspClient::create(ccfg, handler, ec);
    REQUIRE(!ec);
    REQUIRE(client != nullptr);

    REQUIRE(wait_until([&] { return handler->recording.load(); }));
    CHECK(client->is_recording());
    CHECK(rig.stub->saw("ANNOUNCE"));
    CHECK(rig.stub->saw("SETUP"));
    CHECK(rig.stub->saw("RECORD"));

    // ★ 修之前这一句没有任何效果
    client->teardown();

    CHECK_MSG(wait_until([&] { return rig.stub->teardown_seen.load(); }),
              "推流模式 teardown() 没有真正发出 TEARDOWN");
    CHECK(!client->is_recording());

    REQUIRE(wait_until([&] { return handler->closed.load(); }));

    rig.server->stop();
}

// ---------------------------------------------------------------------------
// 2. 401：应用可以在回调里带凭据重发，链路继续走到底
//
//    以前 401 只是 `return`，step 停在原地，而应用没有任何重放入口
//    （状态机变量全在 Impl 私有成员里）—— 实际结局几乎总是干等到超时。
// ---------------------------------------------------------------------------
NETMECH_TEST(client_401_can_be_retried_with_credentials)
{
    Rig rig;
    REQUIRE(rig.start());
    rig.stub->require_auth = true;

    RtspClient::Config ccfg;
    ccfg.mode      = ClientMode::pull;
    ccfg.url       = url_for(rig.port);
    ccfg.transport = TransportPreference::interleaved;

    auto handler = std::make_shared<ClientStub>();
    handler->auto_retry = true;                 // 收到 401 就带 Basic 凭据重发

    std::error_code ec;
    auto client = RtspClient::create(ccfg, handler, ec);
    REQUIRE(!ec);
    REQUIRE(client != nullptr);

    REQUIRE(wait_until([&] { return handler->playing.load(); }));
    CHECK_EQ(handler->unauthorized.load(), 1);
    CHECK(rig.stub->auth_seen.load());
    CHECK(!handler->failed.load());

    client->teardown();
    rig.server->stop();
}

// ---------------------------------------------------------------------------
// 3. 401：不想重试时也能走到终态（原来会永远停在中途）
// ---------------------------------------------------------------------------
NETMECH_TEST(client_401_can_be_aborted)
{
    Rig rig;
    REQUIRE(rig.start());
    rig.stub->require_auth = true;

    RtspClient::Config ccfg;
    ccfg.mode      = ClientMode::pull;
    ccfg.url       = url_for(rig.port);
    ccfg.transport = TransportPreference::interleaved;

    auto handler = std::make_shared<ClientStub>();
    handler->auto_abort = true;

    std::error_code ec;
    auto client = RtspClient::create(ccfg, handler, ec);
    REQUIRE(!ec);
    REQUIRE(client != nullptr);

    REQUIRE(wait_until([&] { return handler->failed.load(); }));
    CHECK_EQ(handler->fail_status.load(), 401);
    CHECK(handler->fail_ec.load() == static_cast<int>(RtspErrc::unauthorized));
    CHECK(handler->fail_stage == "OPTIONS");
    CHECK(!handler->playing.load());

    client->teardown();
    rig.server->stop();
}

// ---------------------------------------------------------------------------
// 4. 异常断链：on_close 必须带上**真实原因**
//
//    以前 on_read 的 ec 被丢掉、close() 里写死 `std::error_code{}` ——
//    应用完全分不清"对端正常 TEARDOWN 走了"和"对端在灌垃圾"。
// ---------------------------------------------------------------------------
NETMECH_TEST(client_on_close_carries_reason)
{
    Rig rig;
    REQUIRE(rig.start());
    rig.stub->send_garbage = true;              // 会话一就绪就发非法报文

    RtspClient::Config ccfg;
    ccfg.mode      = ClientMode::pull;
    ccfg.url       = url_for(rig.port);
    ccfg.transport = TransportPreference::interleaved;

    auto handler = std::make_shared<ClientStub>();
    std::error_code ec;
    auto client = RtspClient::create(ccfg, handler, ec);
    REQUIRE(!ec);
    REQUIRE(client != nullptr);

    REQUIRE(wait_until([&] { return handler->closed.load(); }));
    CHECK_MSG(handler->close_ec.load() != 0,
              "on_close 没带上原因（非法报文应当给出 invalid_message）");
    CHECK(handler->close_ec.load() == static_cast<int>(RtspErrc::invalid_message));

    rig.server->stop();
}

// ---------------------------------------------------------------------------
// 5. 服务端连接表：断开之后 session_count() 必须归零
//
//    以前只有 track、没有 untrack（函数写了但没接线），conns 只进不出，
//    session_count() 每次全表 linear scan，长期运行越来越慢。
// ---------------------------------------------------------------------------
NETMECH_TEST(server_session_count_drops_after_disconnect)
{
    Rig rig;
    REQUIRE(rig.start());

    RtspClient::Config ccfg;
    ccfg.mode      = ClientMode::pull;
    ccfg.url       = url_for(rig.port);
    ccfg.transport = TransportPreference::interleaved;

    auto handler = std::make_shared<ClientStub>();
    std::error_code ec;
    auto client = RtspClient::create(ccfg, handler, ec);
    REQUIRE(!ec);
    REQUIRE(client != nullptr);

    REQUIRE(wait_until([&] { return handler->playing.load(); }));
    CHECK_MSG(wait_until([&] { return rig.server->session_count() >= 1; }),
              "连上之后 session_count 没涨");

    // 主动断开
    client->teardown();
    REQUIRE(wait_until([&] { return handler->closed.load(); }));

    CHECK_MSG(wait_until([&] { return rig.server->session_count() == 0; }),
              "断开之后连接表没销账（untrack 没接线）");

    rig.server->stop();
}

// ---------------------------------------------------------------------------
// URL 里的端口数字溢出：必须拒绝，不能回绕成"看起来合法"的小端口
//
// 以前 `v = v * 10 + digit` 只在循环结束后才判 > 65535，
// 十几位的数字在 unsigned 上会回绕，可能落回 0~65535 并通过校验。
// ---------------------------------------------------------------------------
NETMECH_TEST(client_rejects_overflowing_port)
{
    RtspClient::Config ccfg;
    ccfg.mode = ClientMode::pull;
    ccfg.url  = "rtsp://127.0.0.1:99999999999999/live";

    auto handler = std::make_shared<ClientStub>();
    std::error_code ec;
    auto client = RtspClient::create(ccfg, handler, ec);

    CHECK_MSG(client == nullptr, "溢出的端口号应当直接拒绝");
    CHECK(ec == RtspErrc::invalid_message);

    // 正常的超大但未溢出值也一样
    RtspClient::Config bad;
    bad.url = "rtsp://127.0.0.1:65536/live";
    ec.clear();
    auto c2 = RtspClient::create(bad, std::make_shared<ClientStub>(), ec);
    CHECK(c2 == nullptr);
    CHECK(ec == RtspErrc::invalid_message);
}
