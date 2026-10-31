fix: 会话关闭不清理，导致连接数只增不减 + 内存泄漏

你实测发现：VLC 拉两路后关掉，relay 的「连接 N」一直不降。

根因有两层：

1. **库的接口缺陷**（真正的根因）
   RtspHandler::on_close(std::error_code) 没有 session 参数。
   而一个 handler 通常被**多个会话共用**（中转服务器就是：所有客户端
   连同一个 Relay 实例）。没有这个参数，handler 根本不知道"是哪个
   会话关了"，只能什么都不做。

   改成 on_close(RtspSession&, std::error_code)。

2. **relay 自己也没清理**（被第 1 点掩盖了）
   - on_close 是空实现
   - m_sessions 存的是 shared_ptr<RtspSession> —— session **永远不会
     析构**（它的生命周期本该由 netmech::Conn 持有）
   - 6 个 map<RtspSession*, ...> 只增不删
   - Source::remove_subscriber 是空实现，订阅者列表里留着死连接

   顺带一提：m_sessions 存 shared_ptr 反而**掩盖了**野指针问题。
   如果只删 map 不清 m_sessions，session 被保活但不该再用；
   如果只清 m_sessions 不删订阅者，转发时就是野指针。

修复：
- 库：on_close 加 RtspSession& 参数（session.cpp 用 m_facade 传外层对象）
- relay：m_sessions 改 weak_ptr（session_count 过滤 expired）
- relay：on_close 里清所有表 + 从所有 Source 移除订阅者
- relay：remove_subscriber 真正实现（订阅者空了就放掉 UDP 出口）

实测（3 路推流 + 拉两路再关）：
  拉流时 4 个连接 → TEARDOWN 后回到 3 → 稳定在 3，不再累积
