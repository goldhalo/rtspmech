// =============================================================================
//  rtspmech - 内部：把一条 netmech 连接变成"会话 + 适配器"
// =============================================================================
//  server.cpp 用它；应用看不到（公共头里没有 netmech）。
//  返回的 ConnHandler 交给 netmech 持有，它是会话对象的所有者。
// =============================================================================
#pragma once

#include "rtspmech/export.hpp"

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
RTSPMECH_API std::shared_ptr<netmech::ConnHandler> make_conn_adapter(
    std::shared_ptr<RtspHandler> handler,
    std::function<void(std::shared_ptr<netmech::Conn>)> on_conn,
    std::function<void(netmech::Conn*)>                  on_gone = nullptr);

/// 把任务投递到会话所属连接的 io 线程执行。
///
/// RtspClient 的状态机单线程化用：应用线程入口（teardown/retry/abort）
/// 统一 post 过来，与 on_response 等 io 回调串行，状态无需加锁。
/// allow_inline 语义同 netmech::Conn::post（io 线程上同步执行）。
///
/// @return false = 会话为空 / 无连接 / 任务为空（未投递）。
RTSPMECH_API bool post_to_session(RtspSession& s, std::function<void()> fn,
                                  bool allow_inline = true);

} // namespace detail
} // namespace rtspmech
