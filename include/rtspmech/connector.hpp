// =============================================================================
//  rtspmech - 客户端连接器（连到对端，产出会话）
// =============================================================================
//  文件    : rtspmech/connector.hpp
//  用途    : `RtspServer` 的对侧 —— 主动连出去，连上后把会话交给 handler。
//  依赖    : 标准库 + 本库 session。**公共头不引 netmech**。
//  编码    : UTF-8
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <system_error>

#include <rtspmech/endpoint.hpp>
#include <rtspmech/session.hpp>

namespace netmech {
class Mechanism;   // 前向声明：仅 create_with_mechanism() 用到
} // namespace netmech

namespace rtspmech {

struct RtspConnectorConfig {
    /// 对端地址（含端口）。
    Endpoint remote{"127.0.0.1", 554};

    /// TCP 连接超时。
    std::chrono::milliseconds connect_timeout{3000};
};

/// 主动连出去的 RTSP 客户端端点。
///
/// @par 用法
/// @code
/// auto conn = rtspmech::RtspConnector::create({}, std::make_shared<MyHandler>());
/// if (auto ec = conn->connect()) { /* 立即失败 */ }
/// // 连上后 handler 会收到 on_ready(sess)，在那里发第一个请求
/// @endcode
///
/// @par 线程
/// `connect()` / `close()` / `connected()` 可在任意线程调用；
/// 回调在该连接所属的 io 线程上串行执行。
class RtspConnector {
public:
    struct Impl;   ///< 内部实现（不透明）

    /// 创建（不连接）。io 运行时由本库内部创建。
    /// @param[in] handler 事件处理器（库里以 **shared_ptr 强引用**持有，
    ///                    活到连接关闭为止 —— 不需要应用再存一份）
    static std::shared_ptr<RtspConnector> create(RtspConnectorConfig          cfg,
                                                 std::shared_ptr<RtspHandler> handler);

    /// 复用外部 io 运行时。调用方自己 `#include <netmech/mechanism.hpp>`。
    static std::shared_ptr<RtspConnector> create_with_mechanism(
        std::shared_ptr<netmech::Mechanism> mech, RtspConnectorConfig cfg,
        std::shared_ptr<RtspHandler> handler);

    ~RtspConnector();

    RtspConnector(const RtspConnector&)            = delete;
    RtspConnector& operator=(const RtspConnector&) = delete;

    /// 发起连接（异步）。
    ///
    /// @return 立即失败的原因（参数非法 / 已在连接中 / 底层拒绝发起）；
    ///         空表示已发起，结果通过 `RtspHandler::on_ready`（成功）
    ///         或 `RtspHandler::on_link_failed`（失败）通知。
    [[nodiscard]] std::error_code connect();

    /// 关掉连接（幂等）。
    void close() noexcept;

    [[nodiscard]] bool connected() const noexcept;

private:
    RtspConnector();

    std::shared_ptr<Impl> m_impl;
};

} // namespace rtspmech
