// =============================================================================
//  rtspmech - 客户端连接器实现
// =============================================================================
//  与 server.cpp 对称：自己实现 netmech::Linkable（内部类），应用只给 handler。
// =============================================================================
#include <rtspmech/connector.hpp>

#include "session_internal.hpp"

#include <netmech/conn.hpp>
#include <netmech/linkable.hpp>
#include <netmech/mechanism.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace rtspmech {

namespace {

netmech::Address to_address(const Endpoint& ep)
{
    return netmech::Address(ep.host(), ep.port());
}

} // namespace

// =============================================================================
//  Impl
// =============================================================================

struct RtspConnector::Impl : public std::enable_shared_from_this<RtspConnector::Impl> {
    std::shared_ptr<netmech::Mechanism> mech;
    RtspConnectorConfig                 cfg;
    std::shared_ptr<RtspHandler>        handler;

    std::atomic<bool>                  connecting{false};
    std::atomic<bool>                  connected{false};
    std::shared_ptr<netmech::Linkable> linkable;

    std::mutex                          mu;
    std::shared_ptr<netmech::Conn>      conn;   ///< 连上后由适配器回调填入

    /// netmech 侧：连上 → 造会话（内部适配器）；连不上 → 报给应用。
    class Link final : public netmech::Linkable {
    public:
        explicit Link(std::weak_ptr<Impl> owner) : m_owner(std::move(owner)) {}

        std::shared_ptr<netmech::ConnHandler> on_linkup(netmech::Conn&) override
        {
            auto o = m_owner.lock();
            if (!o) {
                return nullptr;
            }
            // 会话由适配器在 on_open() 里创建；顺手记下连接供 close() 用
            auto adapter = detail::make_conn_adapter(
                o->handler, [w = m_owner](std::shared_ptr<netmech::Conn> conn) {
                    if (auto self = w.lock()) {
                        std::lock_guard<std::mutex> lk(self->mu);
                        self->conn = std::move(conn);
                    }
                });
            if (!adapter) {
                return nullptr;
            }
            o->connected.store(true, std::memory_order_release);
            o->connecting.store(false, std::memory_order_release);
            return adapter;
        }

        void on_linkfail(const netmech::Address&, std::error_code ec) override
        {
            auto o = m_owner.lock();
            if (!o) {
                return;
            }
            o->connecting.store(false, std::memory_order_release);
            o->connected.store(false, std::memory_order_release);
            if (o->handler) {
                o->handler->on_link_failed(ec);
            }
        }

    private:
        std::weak_ptr<Impl> m_owner;
    };

    std::error_code connect();
};

std::error_code RtspConnector::Impl::connect()
{
    if (!mech || !handler) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    bool expected = false;
    if (!connecting.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return std::make_error_code(std::errc::operation_in_progress);
    }

    linkable = std::make_shared<Link>(weak_from_this());

    const auto ms = static_cast<std::uint32_t>(cfg.connect_timeout.count());
    const auto ec = mech->connect(to_address(cfg.remote), linkable, ms);
    if (ec) {
        connecting.store(false, std::memory_order_release);
        linkable.reset();
        return ec;
    }
    return std::error_code{};
}

// =============================================================================
//  RtspConnector
// =============================================================================

RtspConnector::RtspConnector() : m_impl(std::make_shared<Impl>()) {}

RtspConnector::~RtspConnector()
{
    close();
}

std::shared_ptr<RtspConnector> RtspConnector::create(RtspConnectorConfig          cfg,
                                                     std::shared_ptr<RtspHandler> handler)
{
    return create_with_mechanism(netmech::Mechanism::create(), std::move(cfg),
                                 std::move(handler));
}

std::shared_ptr<RtspConnector> RtspConnector::create_with_mechanism(
    std::shared_ptr<netmech::Mechanism> mech, RtspConnectorConfig cfg,
    std::shared_ptr<RtspHandler> handler)
{
    if (!mech || !handler) {
        return nullptr;
    }
    auto c              = std::shared_ptr<RtspConnector>(new RtspConnector());
    c->m_impl->mech     = std::move(mech);
    c->m_impl->cfg      = std::move(cfg);
    c->m_impl->handler  = std::move(handler);
    return c;
}

std::error_code RtspConnector::connect()
{
    return m_impl->connect();
}

void RtspConnector::close() noexcept
{
    m_impl->connected.store(false, std::memory_order_release);
    m_impl->connecting.store(false, std::memory_order_release);

    std::shared_ptr<netmech::Conn> c;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        c = m_impl->conn;
    }
    if (c) {
        c->close();   // 关连接 → 适配器收到 on_close → 会话回调 on_close
    }
}

bool RtspConnector::connected() const noexcept
{
    return m_impl->connected.load(std::memory_order_acquire);
}

} // namespace rtspmech
