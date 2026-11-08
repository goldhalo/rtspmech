// =============================================================================
//  rtspmech - 服务端实现
// =============================================================================
//  RtspServer 自己实现 netmech::Linkable（内部类），应用只提供 RtspAcceptor。
//  这样公共头一行 netmech 都不用引 —— 与 httpmech / rtpmech 的做法一致。
// =============================================================================
#include <rtspmech/server.hpp>

#include "session_internal.hpp"

#include <netmech/conn.hpp>
#include <netmech/linkable.hpp>
#include <netmech/mechanism.hpp>

#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <cstddef>
#include <system_error>
#include <utility>

namespace rtspmech {

namespace {

/// Endpoint → netmech::Address（只在本 TU 用）。
netmech::Address to_address(const Endpoint& ep)
{
    return netmech::Address(ep.host(), ep.port());
}

Endpoint from_address(const netmech::Address& a)
{
    return Endpoint{a.address_string(), a.port()};
}

} // namespace

// =============================================================================
//  Impl
// =============================================================================

struct RtspServer::Impl : public std::enable_shared_from_this<RtspServer::Impl> {
    std::shared_ptr<netmech::Mechanism> mech;
    RtspServerConfig                    cfg;
    std::shared_ptr<RtspAcceptor>       acceptor;

    std::atomic<bool>        running{false};
    Endpoint                 bound;          ///< 实际监听地址
    std::shared_ptr<netmech::Linkable> linkable;

    mutable std::mutex                          mu;
    std::vector<std::weak_ptr<netmech::Conn>>   conns;   ///< 本 server 的连接（stop 时关）

    /// netmech 侧的 Linkable：每条新连接 → 问 acceptor 要 handler → 造会话。
    class Link final : public netmech::Linkable {
    public:
        explicit Link(std::weak_ptr<Impl> owner) : m_owner(std::move(owner)) {}

        std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn& c) override
        {
            auto o = m_owner.lock();
            if (!o || !o->running.load(std::memory_order_acquire)) {
                return nullptr;   // 返回 nullptr = 拒绝，netmech 会关掉这条连接
            }

            // 应用决定"接不接、用哪个 handler"
            std::shared_ptr<RtspHandler> h;
            if (o->acceptor) {
                h = o->acceptor->on_accept(from_address(c.remote_address()));
            }
            if (!h) {
                return nullptr;
            }

            // 适配器由 netmech 持有；它在 on_open() 里造会话、在 on_close() 里通知下线。
            //
            // ★ 必须给 on_gone：以前只记账不销账，`conns` 只进不出 ——
            //   长期运行的服务器里这张表会按"连接历史"增长，而
            //   session_count() 每次都全表 linear scan。
            return detail::make_conn_adapter(
                std::move(h),
                [w = m_owner](std::shared_ptr<netmech::Conn> conn) {
                    if (auto srv = w.lock()) {
                        srv->track(conn);
                    }
                },
                [w = m_owner](netmech::Conn* conn) {
                    if (auto srv = w.lock()) {
                        srv->untrack(conn);
                    }
                });
        }

    private:
        std::weak_ptr<Impl> m_owner;
    };

    std::error_code start();
    void            stop();
    void            track(const std::shared_ptr<netmech::Conn>& c);
    void            untrack(netmech::Conn* c);
};

std::error_code RtspServer::Impl::start()
{
    if (running.exchange(true, std::memory_order_acq_rel)) {
        return std::make_error_code(std::errc::operation_in_progress);   // 已在监听
    }
    if (!mech) {
        running.store(false, std::memory_order_release);
        return std::make_error_code(std::errc::not_connected);
    }

    auto self = weak_from_this();
    linkable  = std::make_shared<Link>(self);

    const auto ec = mech->listen(to_address(cfg.local), linkable, cfg.backlog);
    if (ec) {
        linkable.reset();
        running.store(false, std::memory_order_release);
        return ec;
    }

    if (auto actual = mech->listening_address(to_address(cfg.local))) {
        bound = from_address(*actual);
    } else {
        bound = cfg.local;
    }
    return std::error_code{};
}

void RtspServer::Impl::stop()
{
    if (!running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (mech) {
        mech->unlisten(to_address(bound.port() != 0 ? bound : cfg.local));
    }
    linkable.reset();

    std::vector<std::weak_ptr<netmech::Conn>> victims;
    {
        std::lock_guard<std::mutex> lk(mu);
        victims.swap(conns);
    }
    for (auto& w : victims) {
        if (auto c = w.lock()) {
            c->close();
        }
    }
}

void RtspServer::Impl::track(const std::shared_ptr<netmech::Conn>& c)
{
    std::lock_guard<std::mutex> lk(mu);
    conns.push_back(c);
}

void RtspServer::Impl::untrack(netmech::Conn* c)
{
    std::lock_guard<std::mutex> lk(mu);
    for (auto it = conns.begin(); it != conns.end();) {
        const auto sp = it->lock();
        // 已过期的条目不再有任何意义，顺手清掉（以前只按指针匹配，
        // 过期条目永远匹配不上，表会按连接历史缓慢增长）。
        if (!sp || sp.get() == c) {
            it = conns.erase(it);
            if (sp && sp.get() == c) {
                return;
            }
        } else {
            ++it;
        }
    }
}

// =============================================================================
//  RtspServer
// =============================================================================

RtspServer::RtspServer() : m_impl(std::make_shared<Impl>()) {}

RtspServer::~RtspServer()
{
    stop();
}

std::shared_ptr<RtspServer> RtspServer::create(RtspServerConfig              cfg,
                                               std::shared_ptr<RtspAcceptor> acceptor)
{
    auto mech = netmech::Mechanism::create(netmech::Mechanism::Config{}, cfg.runtime);
    return create_with_mechanism(std::move(mech), std::move(cfg),
                                 std::move(acceptor));
}

std::shared_ptr<RtspServer> RtspServer::create_with_mechanism(
    std::shared_ptr<netmech::Mechanism> mech, RtspServerConfig cfg,
    std::shared_ptr<RtspAcceptor> acceptor)
{
    if (!mech || !acceptor) {
        return nullptr;
    }
    auto s           = std::shared_ptr<RtspServer>(new RtspServer());
    s->m_impl->mech  = std::move(mech);
    s->m_impl->cfg   = std::move(cfg);
    s->m_impl->acceptor = std::move(acceptor);
    return s;
}

std::error_code RtspServer::start()
{
    return m_impl->start();
}

void RtspServer::stop() noexcept
{
    m_impl->stop();
}

Endpoint RtspServer::local() const
{
    return (m_impl->bound.port() != 0) ? m_impl->bound : m_impl->cfg.local;
}

bool RtspServer::running() const noexcept
{
    return m_impl->running.load(std::memory_order_acquire);
}

std::size_t RtspServer::session_count() const noexcept
{
    std::lock_guard<std::mutex> lk(m_impl->mu);
    std::size_t n = 0;
    for (auto& w : m_impl->conns) {
        if (!w.expired()) {
            ++n;
        }
    }
    return n;
}

} // namespace rtspmech
