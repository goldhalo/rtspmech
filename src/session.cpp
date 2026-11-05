// =============================================================================
//  session.cpp —— RtspSession 实现
// =============================================================================
//  读循环要点（netmech 是拉模式）：
//    on_open  → 发起第一笔 read
//    on_read  → 把数据喂给 Framer，派发所有完整帧，再发起下一笔 read
//
//  TCP 的 FIN 只有在下一次 read 时才感知得到，所以读循环不能断——哪怕这一批
//  数据里一条完整消息都没有，也必须立刻再发起读。
// =============================================================================
#include <cstring>
#include <cstdio>
#include <functional>

#include <rtspmech/session.hpp>
#include <rtspmech/common.hpp>

#include <netmech/net.hpp>

#include "framer.hpp"
#include "parser.hpp"

#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

namespace rtspmech {

namespace {

/// 每次 read 提交的缓冲大小。8 KB 对 RTSP 足够（SDP 通常 < 2 KB）。
constexpr std::size_t kReadChunk = 8 * 1024;

} // namespace

// =============================================================================
//  Impl
// =============================================================================

class RtspSession::Impl : public std::enable_shared_from_this<RtspSession::Impl> {
public:
    Impl(std::shared_ptr<netmech::Conn> conn, std::weak_ptr<RtspHandler> handler)
        : m_conn(std::move(conn))
        , m_handler(std::move(handler))
    {
        m_read_buf.resize(kReadChunk);
    }

    bool start()
    {
        if (!m_conn || !m_conn->is_open()) {
            return false;
        }
        m_open.store(true, std::memory_order_release);
        if (!issue_read()) {
            return false;
        }
        // 连接已可读写：通知应用可以发第一个请求了
        if (auto h = m_handler.lock()) {
            h->on_ready(*m_facade);
        }
        return m_open.load(std::memory_order_acquire);
    }

    /// 对端地址（公共 Endpoint，不泄露 netmech 类型）。
    Endpoint peer() const noexcept
    {
        if (!m_conn) {
            return Endpoint{};
        }
        const auto a = m_conn->remote_address();
        return Endpoint{a.address_string(), a.port()};
    }

    /// 处理一批收到的字节。
    void feed(const std::uint8_t* data, std::size_t n)
    {
        m_framer.append(data, n);

        detail::Framer::Frame fr;
        for (;;) {
            const auto st = m_framer.next(fr);
            if (st == detail::Framer::Status::need_more) {
                break;
            }
            if (st == detail::Framer::Status::too_large) {
                // 单条消息超限：这条连接已经不可信（流可能已错位），直接关。
                // 带上原因 —— 应用凭它区分"对端正常走了"和"对端在灌垃圾"。
                close(make_error_code(RtspErrc::too_large));
                return;
            }
            if (fr.kind == detail::Framer::Frame::Kind::binary) {
                dispatch_binary(fr);
            } else {
                dispatch_text(fr);
            }
            if (!m_open.load(std::memory_order_acquire)) {
                return;   // 回调里把会话关了
            }
        }
    }

    bool issue_read()
    {
        if (!m_open.load(std::memory_order_acquire) || !m_conn) {
            return false;
        }
        // 每次都用同一块缓冲：framer 已经把数据拷进自己的缓冲，这块可以复用
        return m_conn->read(netmech::Buf{nullptr, m_read_buf.data(), m_read_buf.size()});
    }

    /// 关闭会话。
    ///
    /// @param[in] reason 关闭原因，原样交给 `RtspHandler::on_close`。
    ///                   空码 = 正常关闭（应用主动关 / 对端正常 TEARDOWN）；
    ///                   非空 = 异常（对端 RST、报文非法、消息超限…）。
    ///
    /// @note 只有**第一次**生效（`m_closed` 挡重复），所以异常原因不会被
    ///       随后的"主动 close()"覆盖掉。
    void close(std::error_code reason = {})
    {
        if (m_closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        // ★ 整个关闭过程中必须让外层 RtspSession 活着。
        //
        //   on_close 是交给应用的回调，应用可能在里面从自己的表里移除
        //   这个 session、触发引用计数归零 —— 如果那一瞬间 session 被
        //   析构，接下来再访问它就是野指针（实测会直接段错误）。
        //
        //   这里先抓一份强引用，保证 close() 返回前对象不会被销毁。
        std::shared_ptr<RtspSession> keep_alive;
        if (m_facade) {
            keep_alive = m_facade->weak_from_this().lock();
        }

        const bool was_open = m_open.exchange(false, std::memory_order_acq_rel);
        if (m_conn) {
            m_conn->close();
        }
        if (was_open) {
            if (auto h = m_handler.lock()) {
                // 用 facade（外层 RtspSession），不是 Impl 自身
                if (m_facade) {
                    h->on_close(*m_facade, reason);
                }
            }
        }
        // keep_alive 在这里析构（可能在 on_close 里已被应用从表里移除）
    }

    /// 发一个请求。CSeq 自动分配（builder 里没设的话）。
    bool send_request(RequestBuilder& req)
    {
        if (!m_open.load(std::memory_order_acquire)) {
            return false;
        }
        req.assign_cseq_if_absent(
            m_next_cseq.fetch_add(1, std::memory_order_relaxed));
        return send_wire(req.build());
    }

    bool send_response(std::string wire) { return send_wire(std::move(wire)); }

    bool send_wire(std::string wire)
    {
        if (!m_open.load(std::memory_order_acquire) || wire.empty() || !m_conn) {
            return false;
        }
        // ConstBuf 的 owner 持有 string，保证写完成前数据有效
        auto holder = std::make_shared<std::string>(std::move(wire));
        return m_conn->write(netmech::ConstBuf{holder, holder->data(), holder->size()});
    }

    bool send_binary(int channel, const std::uint8_t* data, std::size_t len)
    {
        if (!m_open.load(std::memory_order_acquire)) {
            return false;
        }
        if (channel < 0 || channel > 255 || (data == nullptr && len != 0)) {
            return false;
        }
        if (len > 0xFFFF) {
            return false;      // $ 帧头只有 16 位长度
        }

        auto holder = std::make_shared<std::string>();
        holder->resize(4 + len);
        (*holder)[0] = '$';
        (*holder)[1] = static_cast<char>(channel);
        (*holder)[2] = static_cast<char>((len >> 8) & 0xFF);
        (*holder)[3] = static_cast<char>(len & 0xFF);
        if (len != 0) {
            std::memcpy(&(*holder)[4], data, len);
        }
        return m_conn->write(netmech::ConstBuf{holder, holder->data(), holder->size()});
    }

    [[nodiscard]] bool is_open() const noexcept
    {
        return m_open.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t id() const noexcept
    {
        return m_conn ? m_conn->id() : 0;
    }

    [[nodiscard]] netmech::Conn* conn() const noexcept { return m_conn.get(); }

private:
    void dispatch_text(const detail::Framer::Frame& fr)
    {
        // framer 的缓冲是可写的（内部 vector），parser 要原地把 RTSP 换成 HTTP
        auto* mutable_data = const_cast<std::uint8_t*>(fr.data);

        std::error_code ec;
        detail::ParsedMessage pm;
        if (!detail::parse_message(mutable_data, fr.len, pm, ec)) {
            // 解析失败：这条连接上的流已经不可信，关掉（原因带给应用）
            close(ec);
            return;
        }

        auto h = m_handler.lock();
        if (!h) {
            return;
        }

        if (pm.kind == detail::ParsedMessage::Kind::request) {
            RtspRequest req;
            req.method  = pm.method;
            req.uri     = pm.uri;
            req.version = pm.version;
            req.headers = span<const RtspHeader>(pm.headers, pm.num_headers);
            req.body    = span<const std::uint8_t>(pm.body, pm.body_len);
            h->on_request(*m_facade, req);
        } else {
            RtspResponse resp;
            resp.version = pm.version;
            resp.status  = pm.status;
            resp.reason  = pm.reason;
            resp.headers = span<const RtspHeader>(pm.headers, pm.num_headers);
            resp.body    = span<const std::uint8_t>(pm.body, pm.body_len);
            h->on_response(*m_facade, resp);
        }
    }

    void dispatch_binary(const detail::Framer::Frame& fr)
    {
        auto h = m_handler.lock();
        if (!h) {
            return;
        }
        h->on_interleaved(*m_facade, fr.channel, fr.payload(), fr.payload_size());
    }

public:
    void set_facade(RtspSession* f) noexcept { m_facade = f; }

private:
    std::shared_ptr<netmech::Conn> m_conn;
    std::weak_ptr<RtspHandler>     m_handler;
    RtspSession*                   m_facade = nullptr;

    detail::Framer  m_framer;
    std::vector<std::uint8_t> m_read_buf;

    std::atomic<std::uint32_t> m_next_cseq{1};
    std::atomic<bool>          m_open{false};
    std::atomic<bool>          m_closed{false};
};

// =============================================================================
//  RtspSession
// =============================================================================

RtspSession::RtspSession() = default;

RtspSession::~RtspSession()
{
    if (m_impl) {
        m_impl->close();
    }
}

namespace detail {

// =============================================================================
//  ConnAdapter —— netmech 侧真正的 ConnHandler
// =============================================================================
//  以前 RtspSession 自己继承 netmech::ConnHandler，等于把 netmech 的 vtable
//  焊进了公共类型的布局。现在这层关系挪到这里：
//    · 应用完全看不到 netmech；
//    · RtspSession 是普通类，只暴露 send_* / close / 查询。
//  适配器由 netmech 持有（on_accept 的返回值），它再保活会话对象。
class ConnAdapter final : public netmech::ConnHandler {
public:
    ConnAdapter(std::shared_ptr<RtspHandler> handler,
                std::function<void(std::shared_ptr<netmech::Conn>)> on_conn,
                std::function<void(netmech::Conn*)>                  on_gone)
        : m_handler(std::move(handler))
        , m_on_conn(std::move(on_conn))
        , m_on_gone(std::move(on_gone))
    {
    }

    bool on_open(std::shared_ptr<netmech::Conn> conn) override
    {
        // 会话在这里造：netmech 只把 Conn 的 shared_ptr 交给 on_open
        // （on_accept 只给 Conn&），所以"拿连接"必须等到这一刻。
        m_session               = std::shared_ptr<RtspSession>(new RtspSession());
        // ★ 先留一份再 move：下面记账要用同一个连接对象，move 之后它就成了空指针。
        auto conn_keep         = conn;
        m_session->m_impl      = std::make_shared<RtspSession::Impl>(std::move(conn), m_handler);
        m_impl                  = m_session->m_impl;
        m_impl->set_facade(m_session.get());
        const bool ok = m_impl->start();
        // 记账必须在 start() **之后**：start() 里才把会话置为 open，
        // 之前判断 is_open() 永远是 false（Connector 的 close 就找不到连接）。
        if (ok) {
            // 记下裸指针：on_close 时用（netmech 保证回调期间连接对象有效）。
            // 用裸指针而不是 shared_ptr —— 否则适配器自己会把连接多保活一轮，
            // 连接表里的 weak_ptr 永远不过期。
            m_conn_raw = conn_keep.get();
            if (m_on_conn) {
                m_on_conn(conn_keep);   // 交给 server/connector 记账
            }
        }
        return ok;
    }

    void on_read(std::error_code ec, netmech::Buf buf, std::size_t n) override
    {
        if (!m_impl) {
            return;
        }
        if (ec) {
            // ★ 这里是**唯一**能拿到真实断链原因的地方（对端 RST、保活超时…）。
            //   以前 ec 在这里被丢掉，on_close 恒收到空码 —— 应用分不清
            //   "对端正常 TEARDOWN 走了"和"链路被重置，该告警重连"。
            m_impl->close(ec);
            return;
        }
        if (n > 0) {
            m_impl->feed(static_cast<const std::uint8_t*>(buf.data), n);
        }
        // ★ 读循环不能断：FIN 只有在下一次 read 时才感知得到
        if (m_impl->is_open()) {
            m_impl->issue_read();
        }
    }

    void on_write(std::error_code ec, netmech::ConstBuf, std::size_t) override
    {
        if (ec && m_impl) {
            m_impl->close(ec);
        }
    }

    void on_close() override
    {
        if (m_impl) {
            m_impl->close();
        }
        // ★ 反向通知：让 server 把这条连接从记账表里摘掉。
        //   以前只有 on_conn、没有 on_gone，于是 `conns` 只增不减 ——
        //   session_count() 是全表 scan，长期跑起来越来越慢。
        if (m_on_gone) {
            m_on_gone(m_conn_raw);
        }
    }

private:
    std::shared_ptr<RtspHandler>        m_handler;
    std::function<void(std::shared_ptr<netmech::Conn>)> m_on_conn;
    std::function<void(netmech::Conn*)>                  m_on_gone;
    netmech::Conn*                                       m_conn_raw = nullptr;
    std::shared_ptr<RtspSession>        m_session;   ///< 保活会话对象
    std::shared_ptr<RtspSession::Impl>  m_impl;
};

std::shared_ptr<netmech::ConnHandler> make_conn_adapter(
    std::shared_ptr<RtspHandler> handler,
    std::function<void(std::shared_ptr<netmech::Conn>)> on_conn,
    std::function<void(netmech::Conn*)>                  on_gone)
{
    if (!handler) {
        return nullptr;
    }
    return std::make_shared<ConnAdapter>(std::move(handler), std::move(on_conn),
                                         std::move(on_gone));
}

} // namespace detail

bool RtspSession::send_request(RequestBuilder& req)
{
    return m_impl && m_impl->send_request(req);
}

bool RtspSession::send_response(std::string wire)
{
    return m_impl && m_impl->send_response(std::move(wire));
}

bool RtspSession::send_interleaved(int channel, const std::uint8_t* data, std::size_t len)
{
    return m_impl && m_impl->send_binary(channel, data, len);
}

bool RtspSession::is_open() const noexcept
{
    return m_impl && m_impl->is_open();
}

Endpoint RtspSession::peer() const noexcept
{
    return m_impl ? m_impl->peer() : Endpoint{};
}

std::uint64_t RtspSession::id() const noexcept
{
    return m_impl ? m_impl->id() : 0;
}

void RtspSession::close() noexcept
{
    if (m_impl) {
        m_impl->close();
    }
}

} // namespace rtspmech
