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
/// @param[in] on_conn  连接建好时回调（用于记账）
/// @param[in] on_gone  连接关闭时回调，参数是裸指针（**仅在回调期间有效**）。
///                     server 靠它把已断开的连接从表里摘掉 —— 以前只有 on_conn、
///                     没有反向通知，连接表只进不出。
/// @return nullptr 表示 handler 为空。
///
/// @note 连接对象在 `on_open()` 才拿得到（netmech 的 `on_accept` 只给 `Conn&`），
///       所以会话是在那一步才创建的。
std::shared_ptr<netmech::ConnHandler> make_conn_adapter(
    std::shared_ptr<RtspHandler> handler,
    std::function<void(std::shared_ptr<netmech::Conn>)> on_conn,
    std::function<void(netmech::Conn*)>                  on_gone = nullptr);

} // namespace detail
} // namespace rtspmech
