// =============================================================================
//  rtspmech - 服务端（监听 + 接连接）
// =============================================================================
//  文件    : rtspmech/server.hpp
//  用途    : 监听端口、接受连接、为每条连接造一个 `RtspSession`。
//  依赖    : 标准库 + 本库 session。**公共头不引 netmech**。
//  编码    : UTF-8
//
//  ★ 为什么需要这一层
//    以前应用必须自己实现 `netmech::Linkable`、在 `on_accept` 里
//    `RtspSession::create(conn, ...)` 再把会话返回给 netmech —— 因为
//    `RtspSession` 当时**继承**了 `netmech::ConnHandler`。这让 rtspmech 的
//    使用者必须认识 netmech，装出去也得带 netmech 的头。
//
//    现在反过来：`RtspServer` 自己实现 Linkable（内部类），accept 之后
//    造会话 + 内部适配器。应用只需要实现 `RtspAcceptor`（每来一条连接，
//    返回一个 `RtspHandler`），完全不碰 netmech。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

#include <rtspmech/endpoint.hpp>
#include <rtspmech/session.hpp>

namespace netmech {
class Mechanism;   // 前向声明：仅 create_with_mechanism() 用到
} // namespace netmech

namespace rtspmech {

struct RtspServerConfig {
    /// 监听地址。端口 0 表示由系统分配，起来后用 `local()` 取实际端口。
    Endpoint local{"0.0.0.0", 554};

    /// listen backlog。
    int backlog = 64;
};

/// 新连接的"要不要接、用哪个 handler"由应用决定。
class RtspAcceptor {
public:
    virtual ~RtspAcceptor() = default;

    /// 新连接到来（在 io 线程上调用，**不要阻塞**）。
    ///
    /// @param[in] peer 对端地址
    /// @return 处理这个连接的 handler；**返回 nullptr 表示拒绝**（库会关掉连接）。
    ///
    /// @note handler 由库以 weak_ptr 持有 —— 应用自己管生命周期，
    ///       想让它活到会话结束就自己存好。
    /// @note 回调里不要做耗时操作：它会挡住该 io 线程上的其它连接。
    virtual std::shared_ptr<RtspHandler> on_accept(const Endpoint& peer) = 0;
};

/// RTSP 服务端。
///
/// @par 用法
/// @code
/// class MyAcceptor : public rtspmech::RtspAcceptor {
/// public:
///     std::shared_ptr<rtspmech::RtspHandler>
///     on_accept(const rtspmech::Endpoint&) override {
///         return std::make_shared<MyHandler>();   // 每个连接一个
///     }
/// };
///
/// rtspmech::RtspServerConfig cfg;
/// cfg.local = {"0.0.0.0", 554};
///
/// auto srv = rtspmech::RtspServer::create(cfg, std::make_shared<MyAcceptor>());
/// if (auto ec = srv->start()) { /* ... */ }
/// @endcode
///
/// @par 线程
/// `start()` / `stop()` / `local()` 可在任意线程调用；`on_accept` 在该连接
/// 所属的 io 线程上串行执行。
class RtspServer {
public:
    struct Impl;   ///< 内部实现（不透明）

    /// 创建（不监听）。io 运行时由本库内部创建。
    static std::shared_ptr<RtspServer> create(RtspServerConfig cfg,
                                              std::shared_ptr<RtspAcceptor> acceptor);

    /// 复用外部 io 运行时（一个进程里多个库共用一份线程池时用）。
    /// 调用方自己 `#include <netmech/mechanism.hpp>`。
    static std::shared_ptr<RtspServer> create_with_mechanism(
        std::shared_ptr<netmech::Mechanism> mech, RtspServerConfig cfg,
        std::shared_ptr<RtspAcceptor> acceptor);

    ~RtspServer();

    RtspServer(const RtspServer&)            = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    /// 开始监听。重复调用返回 `RtspErrc::already_started`（若存在）或
    /// 一个通用错误码。
    [[nodiscard]] std::error_code start();

    /// 停止监听并关闭本 server 建立的所有连接。幂等。
    ///
    /// @warning 不要在本 server 的会话回调里调用。
    void stop() noexcept;

    /// 实际监听地址（`cfg.local.port == 0` 时能拿到系统分配的端口）。
    [[nodiscard]] Endpoint local() const;

    [[nodiscard]] bool        running() const noexcept;
    [[nodiscard]] std::size_t session_count() const noexcept;

private:
    RtspServer();

    std::shared_ptr<Impl> m_impl;
};

} // namespace rtspmech
