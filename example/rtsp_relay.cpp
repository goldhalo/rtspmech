// =============================================================================
//  rtsp_relay —— RTSP 收流 + 转发（中转服务器）
// =============================================================================
//  用法：rtsp_relay [端口]
//
//  两个角色：
//    * **收流端**：接 100 个推流客户端（ANNOUNCE → SETUP → RECORD）
//        - UDP 推流：服务端分配一个 UDP 端口收 RTP
//        - TCP 推流：从 interleaved 通道收
//    * **分发端**：对外提供拉流（DESCRIBE → SETUP → PLAY）
//        - ffmpeg 随时可以连上来拉任意一路
//
//  每路推流用不同的 URL（/push/0 ... /push/99）。
//  拉流用 `/play/<id>` 对应该路的源。
//
//  转发策略：**原样转发**（保留源的 seq/ts/ssrc）。新订阅者从下一个包开始
//  收（不补历史），seq 从中间开始是正常的，ffmpeg 能处理。
// =============================================================================
#include <rtspmech/client.hpp>
#include <rtspmech/rtspmech.hpp>

#include <netmech/net.hpp>
#include <netmech/udp.hpp>
#include <netmech/worker_pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rtspmech;
using namespace netmech;

namespace {

using namespace std::chrono_literals;

/// 一个 RTP 包（转发用）。
struct RtpPacket {
    std::vector<std::uint8_t> data;   ///< 完整 RTP 包（含头）
};

/// 跨线程用的请求副本。
///
/// `RtspRequest` 里的字段都是借用视图，回调返回即失效，投递到线程池前
/// 必须拷成自己的数据。
struct OwnedRequest {
    std::string method;
    std::string uri;
    std::uint32_t cseq = 0;
    std::string transport;
    std::string body;

    explicit OwnedRequest(const RtspRequest& r)
        : method(r.method.data(), r.method.size())
        , uri(r.uri.data(), r.uri.size())
        , cseq(r.cseq())
        , transport(r.header("Transport").data(), r.header("Transport").size())
        , body(reinterpret_cast<const char*>(r.body.data()), r.body.size())
    {}
};

// =============================================================================
//  一个"源"：推流端送进来的流，以及它的订阅者
// =============================================================================

class Source {
public:
    explicit Source(std::string id) : m_id(std::move(id)) {}

    const std::string& id() const { return m_id; }

    void set_sdp(std::string sdp)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_sdp = std::move(sdp);
    }

    std::string sdp() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_sdp;
    }

    // ---- 推流端（只有一个）----

    void set_publisher(RtspSession* s, bool udp, int channel)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pub_session = s;
        m_pub_udp     = udp;
        m_pub_channel = channel;
    }

    // ---- 订阅者（多个）----

    struct Subscriber {
        RtspSession*  session = nullptr;
        bool          udp     = false;
        int           channel = 0;
        std::string   ip;
        std::uint16_t port = 0;
    };

    void add_subscriber(const Subscriber& sub)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_subs.push_back(sub);
    }

    void remove_subscriber(netmech::Conn* conn)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // 用 session 的 conn 指针不好比，这里简化为"会话关闭时整路清理"
        // 生产实现应该给每个订阅者一个 id
        (void)conn;
    }

    /// 设置 UDP 转发出口（所有 UDP 订阅者共用这一个 socket）。
    void set_udp_out(std::shared_ptr<netmech::UdpEndpoint> ep)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_udp_out = std::move(ep);
    }

    /// 收到一个 RTP 包 → 转发给所有订阅者。
    void forward(const std::uint8_t* rtp, std::size_t n)
    {
        std::lock_guard<std::mutex> lk(m_mtx);

        // ★ UDP 的 send_to 是异步的，owner 为空时库不持有数据。
        //   rtp 指向 on_read 的接收缓冲，回调返回就失效 —— 必须由我们持有。
        //   多个 UDP 订阅者共享同一份拷贝，避免每个都 memcpy。
        std::shared_ptr<std::vector<std::uint8_t>> holder;
        const bool has_udp_sub = std::any_of(m_subs.begin(), m_subs.end(),
                                             [](const Subscriber& s) { return s.udp; });
        if (has_udp_sub && m_udp_out) {
            holder = std::make_shared<std::vector<std::uint8_t>>(rtp, rtp + n);
        }

        for (const auto& s : m_subs) {
            if (s.udp) {
                if (holder && !s.ip.empty() && s.port != 0) {
                    m_udp_out->send_to(netmech::Address{s.ip, s.port},
                                       netmech::ConstBuf{holder, holder->data(),
                                                         holder->size()});
                }
                continue;
            }
            if (!s.session) {
                continue;
            }
            // TCP 的 send_interleaved 内部会拷贝，直接用原始指针即可
            s.session->send_interleaved(s.channel, rtp, n);
        }
    }

    std::size_t subscriber_count() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_subs.size();
    }

private:
    std::string m_id;
    std::uint16_t m_port;
    std::shared_ptr<netmech::WorkerPool> m_pool;
    std::map<RtspSession*, std::size_t>  m_session_index;
    std::size_t                          m_next_session_index = 0;
    mutable std::mutex m_mtx;
    std::string m_sdp;

    RtspSession* m_pub_session = nullptr;
    bool         m_pub_udp = false;
    int          m_pub_channel = 0;

    std::shared_ptr<netmech::UdpEndpoint> m_udp_out;
    std::vector<Subscriber> m_subs;
};

// =============================================================================
//  中继服务器
// =============================================================================

class Relay : public netmech::Linkable,
              public RtspHandler,
              public std::enable_shared_from_this<Relay> {
public:
    Relay(std::uint16_t listen_port, std::shared_ptr<netmech::WorkerPool> pool)
        : m_port(listen_port), m_pool(std::move(pool)) {}

    std::shared_ptr<ConnHandler> on_accept(Conn& c) override
    {
        std::error_code ec;
        auto self = shared_from_this();
        std::shared_ptr<RtspHandler> h(self, static_cast<RtspHandler*>(this));
        std::weak_ptr<RtspHandler>   wh = h;
        auto s = RtspSession::create(c.shared_from_this(), wh, ec);
        if (ec) {
            return nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_sessions.push_back(s);
            m_session_ip[s.get()] = c.remote_address().address_string();
        }
        std::printf("[relay] 客户端连上（共 %zu）\n", session_count());
        return s;
    }

    // ---- RtspHandler ----
    //
    // 一个 Relay 同时处理推流端和拉流端。靠 URL 前缀区分：
    //   /push/<id>  → 推流
    //   /play/<id>  → 拉流
    //
    // 但 OPTIONS/DESCRIBE 时还不知道意图，所以靠首个 ANNOUNCE（推流）或
    // SETUP（拉流）来判断。

    void on_request(RtspSession& sess, const RtspRequest& req) override
    {
        // ★ 信令处理投递到业务线程池。
        //
        //   为什么这里要 post：RTSP 的 on_request 跑在 io 线程上，而
        //   SETUP 要解析 Transport、可能要开 UDP 端口，DESCRIBE 要拷 SDP。
        //   这些虽然不慢，但一旦以后加上认证/鉴权/数据库查询，就会直接
        //   卡住所有连接的收发。
        //
        //   用 post_to(session_id) 保证同一个会话的请求按序处理
        //   （OPTIONS → ANNOUNCE → SETUP → RECORD 不能乱序）。
        //
        //   媒体转发（on_interleaved）**不**走线程池：那是纯 IO，
        //   在 io 线程做延迟最低（20000 包/秒时线程跳转的代价很可观）。
        const std::size_t idx = session_index(sess);
        m_pool->post_to(idx, [this, &sess, req = OwnedRequest(req)]() mutable {
            handle_request(sess, req);
        });
    }

    void handle_request(RtspSession& sess, const OwnedRequest& req)
    {
        const std::string& method = req.method;
        const std::string& uri    = req.uri;
        std::printf("[relay] <- %s %s\n", method.c_str(), uri.c_str());
        std::fflush(stdout);

        if (method == "OPTIONS") {
            reply(sess, req.cseq, ResponseBuilder(200, "OK")
                                .header("Public", "OPTIONS, ANNOUNCE, DESCRIBE, SETUP, PLAY, RECORD, TEARDOWN"));
        } else if (method == "ANNOUNCE") {
            // 推流：记下 SDP，建 source
            const std::string id = stream_id(uri);
            auto src = get_or_create_source(id);
            src->set_sdp(std::string(reinterpret_cast<const char*>(req.body.data()),
                                     req.body.size()));
            reply(sess, req.cseq, ResponseBuilder(200, "OK"));
        } else if (method == "DESCRIBE") {
            const std::string id = stream_id(uri);
            auto src = get_source(id);
            if (!src) {
                reply(sess, req.cseq, ResponseBuilder(404, "Not Found"));
                return;
            }
            reply(sess, req.cseq, ResponseBuilder(200, "OK")
                                .header("Content-Type", "application/sdp")
                                .header("Content-Base",
                                        "rtsp://127.0.0.1:" + std::to_string(m_port) +
                                            "/" + id + "/")
                                .body(src->sdp()));
        } else if (method == "SETUP") {
            handle_setup(sess, req.cseq, req.transport, uri);
        } else if (method == "RECORD") {
            reply(sess, req.cseq, ResponseBuilder(200, "OK").header("Session", session_id_of(sess)));
            mark_publisher(sess, uri, true);
        } else if (method == "PLAY") {
            reply(sess, req.cseq, ResponseBuilder(200, "OK")
                                .header("Session", session_id_of(sess))
                                .header("Range", "npt=0.000-"));
            mark_player(sess, uri);
        } else if (method == "TEARDOWN") {
            reply(sess, req.cseq, ResponseBuilder(200, "OK"));
        } else {
            reply(sess, req.cseq, ResponseBuilder(501, "Not Implemented"));
        }
    }

    void on_interleaved(RtspSession& sess, int ch, const std::uint8_t* d, std::size_t n) override
    {
        (void)ch;
        // 只有**推流端**的包才转发；订阅者不会往我们发媒体
        Source* src = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_roles[&sess] == Role::publisher) {
                auto it = m_pub_stream.find(&sess);
                if (it != m_pub_stream.end()) {
                    auto sit = m_sources.find(it->second);
                    if (sit != m_sources.end()) {
                        src = sit->second.get();
                    }
                }
            }
        }
        if (src) {
            src->forward(d, n);
            ++m_forwarded;
        }
    }

    void on_close(std::error_code) override {}

    std::size_t session_count() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_sessions.size();
    }

    std::uint64_t forwarded() const { return m_forwarded.load(); }

private:
    /// 给会话分配一个稳定的序号（用于 post_to 保序）。
    std::size_t session_index(RtspSession& sess)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_session_index.find(&sess);
        if (it != m_session_index.end()) {
            return it->second;
        }
        const std::size_t idx = m_next_session_index++;
        m_session_index[&sess] = idx;
        return idx;
    }

    /// 从 URI 里取流 ID。
    ///
    ///   `/push/12`              → `12`
    ///   `/push/12/trackID=0`    → `12`   ← SETUP 用的是这种（带 track）
    ///   `/play/12/trackID=0`    → `12`
    static std::string stream_id(const std::string& uri)
    {
        std::string s = uri;
        // 1) 去掉末尾的 /trackID=xxx（SETUP 时客户端会带上）
        const auto tid = s.rfind("/trackID=");
        if (tid != std::string::npos) {
            s = s.substr(0, tid);
        }
        // 2) 去掉末尾的斜杠 —— PLAY 的 URI 常是 `.../play/0/`，
        //    不处理的话 rfind('/') 会指到最后一个字符，取出来是空串。
        while (!s.empty() && s.back() == '/') {
            s.pop_back();
        }
        // 3) 取最后一段
        const auto slash = s.rfind('/');
        if (slash == std::string::npos) {
            return s;
        }
        return s.substr(slash + 1);
    }

    std::shared_ptr<Source> get_or_create_source(const std::string& id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_sources.find(id);
        if (it != m_sources.end()) {
            return it->second;
        }
        auto src = std::make_shared<Source>(id);
        m_sources[id] = src;
        return src;
    }

    std::shared_ptr<Source> get_source(const std::string& id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_sources.find(id);
        return (it == m_sources.end()) ? nullptr : it->second;
    }

    void handle_setup(RtspSession& sess, std::uint32_t cseq,
                      const std::string& transport_header, const std::string& uri)
    {
        RtspTransport t;
        if (!parse_transport(transport_header, t)) {
            reply(sess, cseq, ResponseBuilder(400, "Bad Request"));
            return;
        }

        const std::string id = stream_id(uri);
        auto src = get_or_create_source(id);

        if (t.mode == TransportMode::interleaved) {
            reply(sess, cseq, ResponseBuilder(200, "OK")
                                .header("Transport",
                                        "RTP/AVP/TCP;unicast;interleaved=" +
                                            std::to_string(t.ch1) + "-" +
                                            std::to_string(t.ch2))
                                .header("Session", session_id_of(sess)));
            // 先把会话记进表，RECORD / PLAY 时才确定角色
            set_session_stream(&sess, id, true, t.ch1);
        } else {
            // UDP：分配一个服务端端口
            std::uint16_t p = alloc_udp_port();
            std::uint16_t p_actual = p;

            // 推流端：我们要在这个端口上**收** RTP
            if (uri.find("/push/") != std::string::npos) {
                // ★ 端口可能被占（上一次运行的残留、别的服务）。
                //   探测到失败就换下一个，最多试 20 次。
                bool ok = false;
                std::uint16_t actual = p;
                for (int tryn = 0; tryn < 20 && !ok; ++tryn) {
                    actual = static_cast<std::uint16_t>(50000 + (m_next_udp_port++ % 10000));
                    ok = create_udp_ingress(actual, src);
                }
                if (!ok) {
                    reply(sess, cseq, ResponseBuilder(500, "No UDP Port"));
                    return;
                }
                p_actual = actual;
            } else {
                // 拉流端：记下它的 client_port 和源 IP，转发时用
                std::lock_guard<std::mutex> lk(m_mtx);
                auto it = m_session_ip.find(&sess);
                const std::string ip =
                    (it != m_session_ip.end()) ? it->second : "127.0.0.1";
                m_player_udp[&sess] = {ip, t.client_rtp_port};
            }

            reply(sess, cseq, ResponseBuilder(200, "OK")
                                .header("Transport",
                                        "RTP/AVP;unicast;client_port=" +
                                            std::to_string(t.client_rtp_port) + "-" +
                                            std::to_string(t.client_rtcp_port) +
                                            ";server_port=" + std::to_string(p_actual) + "-" +
                                            std::to_string(p_actual + 1))
                                .header("Session", session_id_of(sess)));
            set_session_stream(&sess, id, false, 0);
        }
    }

    void mark_publisher(RtspSession& sess, const std::string& uri, bool)
    {
        const std::string id = stream_id(uri);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_roles[&sess] = Role::publisher;
            m_pub_stream[&sess] = id;
        }
        auto src = get_or_create_source(id);
        const bool udp = !m_session_interleaved[&sess];
        src->set_publisher(&sess, udp, m_session_channel[&sess]);
        std::printf("[relay] 推流开始: %s（%s）\n", id.c_str(), udp ? "UDP" : "TCP");
        std::fflush(stdout);
    }

    void mark_player(RtspSession& sess, const std::string& uri)
    {
        const std::string id = stream_id(uri);
        auto src = get_source(id);
        if (!src) {
            std::printf("[relay] 拉流失败: 没有源 %s\n", id.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_roles[&sess] = Role::player;
        }
        Source::Subscriber sub;
        sub.session = &sess;
        sub.udp     = !m_session_interleaved[&sess];
        sub.channel = m_session_channel[&sess];
        if (sub.udp) {
            std::lock_guard<std::mutex> lk(m_mtx);
            auto it = m_player_udp.find(&sess);
            if (it != m_player_udp.end()) {
                sub.ip   = it->second.first;
                sub.port = it->second.second;
            }
            // UDP 出口（所有 UDP 订阅者共用）
            if (!m_udp_out) {
                m_udp_out = create_udp_out();
                src->set_udp_out(m_udp_out);
            }
        }
        src->add_subscriber(sub);
        std::printf("[relay] 拉流开始: %s（%s，通道 %d，目标 %s:%u，当前 %zu 个订阅者）\n",
                    id.c_str(), sub.udp ? "UDP" : "TCP", sub.channel,
                    sub.ip.c_str(), static_cast<unsigned>(sub.port),
                    src->subscriber_count());
        std::fflush(stdout);
    }

    enum class Role { unknown, publisher, player };

    /// UDP 收流的 handler：把收到的 RTP 转给对应的 Source。
    class UdpIngress : public netmech::UdpHandler {
    public:
        explicit UdpIngress(std::shared_ptr<Source> src) : m_src(std::move(src)) {}

        void on_read(netmech::UdpEndpoint&, std::error_code ec, netmech::Buf buf,
                     std::size_t n, const netmech::Address&) override
        {
            if (ec || n == 0) {
                return;
            }
            m_src->forward(static_cast<const std::uint8_t*>(buf.data), n);
            ++count;
        }

        std::atomic<std::uint64_t> count{0};

    private:
        std::shared_ptr<Source> m_src;
    };

    /// 给一路推流创建 UDP 收流端点。
    bool create_udp_ingress(std::uint16_t port, const std::shared_ptr<Source>& src)
    {
        netmech::UdpConfig cfg;
        cfg.bind = netmech::Address::any(port);
        cfg.recv_capacity = 2048;

        auto handler = std::make_shared<UdpIngress>(src);
        std::error_code ec;
        auto ep = netmech::create_udp_endpoint(cfg, handler, nullptr, ec);
        if (!ep) {
            std::printf("[relay] UDP 收流端点 %u 创建失败: %s\n",
                        port, ec.message().c_str());
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_udp_ingress[port] = {ep, handler};
        }
        std::printf("[relay] UDP 收流端点已开: %u\n", port);
        std::fflush(stdout);
        return true;
    }

    void set_session_stream(RtspSession* sess, const std::string& id, bool interleaved, int ch)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_session_stream[sess] = id;
        m_session_interleaved[sess] = interleaved;
        m_session_channel[sess] = ch;
    }

    Source* publisher_source(RtspSession* sess)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_session_stream.find(sess);
        if (it == m_session_stream.end()) {
            return nullptr;
        }
        auto sit = m_sources.find(it->second);
        return (sit == m_sources.end()) ? nullptr : sit->second.get();
    }

    std::string session_id_of(RtspSession& sess)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_session_ids.find(&sess);
        if (it != m_session_ids.end()) {
            return it->second;
        }
        const std::string id = "s" + std::to_string(++m_next_session_id);
        m_session_ids[&sess] = id;
        return id;
    }

    /// 创建一个用于向订阅者发 RTP 的 UDP 端点（bind 到临时端口）。
    std::shared_ptr<netmech::UdpEndpoint> create_udp_out()
    {
        struct NullHandler : netmech::UdpHandler {
            void on_read(netmech::UdpEndpoint&, std::error_code, netmech::Buf,
                         std::size_t, const netmech::Address&) override {}
        };
        netmech::UdpConfig cfg;
        cfg.bind = netmech::Address::any(0);
        cfg.recv_capacity = 2048;
        std::error_code ec;
        auto ep = netmech::create_udp_endpoint(cfg, std::make_shared<NullHandler>(),
                                               nullptr, ec);
        if (!ep) {
            std::printf("[relay] UDP 出口创建失败: %s\n", ec.message().c_str());
        } else {
            std::printf("[relay] UDP 出口已开: %s\n", ep->local_address().to_string().c_str());
        }
        return ep;
    }

    std::uint16_t alloc_udp_port()
    {
        return static_cast<std::uint16_t>(50000 + (m_next_udp_port++ % 10000));
    }

    void reply(RtspSession& sess, std::uint32_t cseq, ResponseBuilder b)
    {
        b.cseq(cseq);
        sess.send_response(b.build());
    }

    std::uint16_t m_port;
    std::shared_ptr<netmech::WorkerPool> m_pool;
    std::map<RtspSession*, std::size_t>  m_session_index;
    std::size_t                          m_next_session_index = 0;
    mutable std::mutex m_mtx;
    std::map<std::string, std::shared_ptr<Source>> m_sources;
    std::vector<std::shared_ptr<RtspSession>>      m_sessions;
    std::map<RtspSession*, std::string>            m_session_ids;
    std::map<RtspSession*, std::string>            m_session_stream;
    std::map<RtspSession*, bool>                   m_session_interleaved;
    std::map<RtspSession*, int>                    m_session_channel;
    std::map<RtspSession*, Role>                   m_roles;
    std::map<RtspSession*, std::string>            m_session_ip;
    std::map<RtspSession*, std::pair<std::string, std::uint16_t>> m_player_udp;
    std::map<std::uint16_t, std::pair<std::shared_ptr<netmech::UdpEndpoint>,
                                      std::shared_ptr<UdpIngress>>> m_udp_ingress;
    std::shared_ptr<netmech::UdpEndpoint>          m_udp_out;
    std::map<RtspSession*, std::string>            m_pub_stream;
    std::uint32_t                                  m_next_session_id = 0;
    std::uint32_t                                  m_next_udp_port = 0;
    std::atomic<std::uint64_t>                     m_forwarded{0};
};

} // namespace

int main(int argc, char** argv)
{
    const std::uint16_t port =
        (argc > 1) ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 8554;

    if (!netmech::initialize(4)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }

    // 业务线程池：信令处理走这里，io 线程只负责收发
    netmech::WorkerPool::Config pcfg;
    pcfg.threads = 4;
    std::error_code pec;
    auto pool = netmech::WorkerPool::create(pcfg, pec);
    if (!pool) {
        std::fprintf(stderr, "WorkerPool 创建失败: %s\n", pec.message().c_str());
        return 1;
    }
    std::printf("[relay] 业务线程池: %zu 线程\n", pool->thread_count());

    auto mech  = Mechanism::create();
    auto relay = std::make_shared<Relay>(port, pool);

    std::error_code ec;
    mech->set_linkable(relay);
    if ((ec = mech->listen(Address{"0.0.0.0", port}, relay))) {
        std::fprintf(stderr, "listen: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("[relay] 监听 rtsp://127.0.0.1:%u\n", port);
    std::printf("[relay] 推流：rtsp://127.0.0.1:%u/push/<id>\n", port);
    std::printf("[relay] 拉流：rtsp://127.0.0.1:%u/play/<id>\n", port);
    std::printf("[relay] 每 5 秒打印一次统计\n\n");

    for (int i = 0; i < 3600; ++i) {
        std::this_thread::sleep_for(1s);
        if (i % 5 == 4) {
            std::printf("[relay] 连接 %zu，转发 %llu 包\n",
                        relay->session_count(),
                        static_cast<unsigned long long>(relay->forwarded()));
            std::fflush(stdout);
        }
    }

    mech->close_all();
    netmech::shutdown();
    return 0;
}
