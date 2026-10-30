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
#include <rtspmech/session.hpp>
#include <rtspmech/common.hpp>

#include <netmech/net.hpp>

#include "framer.hpp"
#include "parser.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace rtspmech {

namespace {

/// 每次 read 提交的缓冲大小。8 KB 对 RTSP 足够（SDP 通常 < 2 KB）。
constexpr std::size_t kReadChunk = 8 * 1024;

} // namespace

// =============================================================================
//  Impl
// =============================================================================

class RtspSession::Impl {
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
                // 单条消息超限：这条连接已经不可信（流可能已错位），直接关
                close();
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

    void close()
    {
        if (m_closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const bool was_open = m_open.exchange(false, std::memory_order_acq_rel);
        if (m_conn) {
            m_conn->close();
        }
        if (was_open) {
            if (auto h = m_handler.lock()) {
                h->on_close(std::error_code{});
            }
        }
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
            // 解析失败：这条连接上的流已经不可信，关掉
            close();
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

std::shared_ptr<RtspSession> RtspSession::create(std::shared_ptr<netmech::Conn> conn,
                                                 std::weak_ptr<RtspHandler>     handler,
                                                 std::error_code&               ec)
{
    ec.clear();

    if (!conn) {
        ec = make_error_code(RtspErrc::invalid_message);
        return nullptr;
    }
    if (handler.expired()) {
        ec = make_error_code(RtspErrc::invalid_message);
        return nullptr;
    }

    auto session = std::shared_ptr<RtspSession>(new RtspSession());
    session->m_impl = std::make_shared<Impl>(std::move(conn), std::move(handler));
    session->m_impl->set_facade(session.get());
    return session;
}

bool RtspSession::on_open(std::shared_ptr<netmech::Conn> conn)
{
    (void)conn;   // conn 已在 create 时持有
    if (!m_impl) {
        return false;
    }
    return m_impl->start();
}

void RtspSession::on_read(std::error_code ec, netmech::Buf buf, std::size_t n)
{
    (void)buf;
    if (!m_impl) {
        return;
    }
    if (ec) {
        m_impl->close();
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

void RtspSession::on_write(std::error_code ec, netmech::ConstBuf buf, std::size_t n)
{
    (void)buf;
    (void)n;
    if (ec && m_impl) {
        m_impl->close();
    }
}

void RtspSession::on_close()
{
    if (m_impl) {
        m_impl->close();
    }
}

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
