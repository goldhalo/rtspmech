// =============================================================================
//  rtspmech - 内部：把一条 netmech 连接变成"会话 + 适配器"
// =============================================================================
//  server.cpp 用它；应用看不到（公共头里没有 netmech）。
//  返回的 ConnHandler 交给 netmech 持有，它是会话对象的所有者。
// =============================================================================
#pragma once

#include <memory>
#include <functional>

#include <netmech/iohandler.hpp>

#include <rtspmech/session.hpp>

namespace rtspmech {
namespace detail {

/// 造一个 netmech 侧的 handler（内部持有会话）。
///
/// @param[in] handler 应用提供的 RTSP 事件处理器（**强引用**：连接活多久就活多久）
/// @return nullptr 表示 handler 为空。
///
/// @note 连接对象在 `on_open()` 才拿得到（netmech 的 `on_accept` 只给 `Conn&`），
///       所以会话是在那一步才创建的。
std::shared_ptr<netmech::ConnHandler> make_conn_adapter(
    std::shared_ptr<RtspHandler> handler,
    std::function<void(std::shared_ptr<netmech::Conn>)> on_conn);

} // namespace detail
} // namespace rtspmech
