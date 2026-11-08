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

#include "rtspmech/export.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

#include <rtspmech/endpoint.hpp>
#include <rtspmech/session.hpp>

namespace netmech {
class Mechanism;   // 前向声明：仅 create_with_mechanism() 用到
class Runtime;     // 前向声明：Config::runtime
} // namespace netmech

namespace rtspmech {

struct RtspServerConfig {
    /// 监听地址。端口 0 表示由系统分配，起来后用 `local()` 取实际端口。
    Endpoint local{"0.0.0.0", 554};

    /// listen backlog。
    int backlog = 64;

    /// 注入 netmech 的 io 运行时（`netmech/runtime.hpp`）。
    /// 空 = 进程级默认运行时；非空 = 本 server 的 accept/连接都跑在该 Runtime 上。
    /// @note 本头只前向声明；创建 Runtime 需包含 `<netmech/runtime.hpp>`。
    std::shared_ptr<netmech::Runtime> runtime;
};

/// 新连接的"要不要接、用哪个 handler"由应用决定。
class RTSPMECH_API RtspAcceptor {
public:
    virtual ~RtspAcceptor() = default;

    /// 新连接到来（在 io 线程上调用，**不要阻塞**）。
    ///
    /// @param[in] peer 对端地址
    /// @return 处理这个连接的 handler；**返回 nullptr 表示拒绝**（库会关掉连接）。
    ///
    /// @note handler 由库以 **shared_ptr 强引用**持有，活到连接关闭为止 ——
    ///       应用**不需要**自己再存一份（存了也不会有问题，只是多一个引用）。
    ///       想在会话结束前就释放它，请调 `RtspSession::close()`：
    ///       关闭后适配器随即释放强引用。
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
class RTSPMECH_API RtspServer {
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

    /// 开始监听。
    ///
    /// @return 空 = 成功；已在监听时返回 `std::errc::operation_in_progress`
    ///         （幂等场景可以直接忽略）；其余为系统错误。
    ///
    /// @note 以前这里写的是"返回 `RtspErrc::already_started`"，但那个错误码
    ///       **根本不存在** —— 照文档写 `if (ec == RtspErrc::already_started)`
    ///       连编译都过不了。
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
