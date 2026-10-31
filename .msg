feat: 新增 RtspClient（完整的 OPTIONS→DESCRIBE→SETUP→PLAY 状态机）

这套流程每个 RTSP 应用都要写一遍，放库里比放示例里省事。

- 状态机：OPTIONS → DESCRIBE → SETUP（每个 track 一次）→ PLAY
- 从 SDP 的 a=control: 提取 track 列表（含绝对 URL 和相对 id 两种写法）
- Transport 协商：UDP（client_port）或 interleaved（channel）
- Session 从 SETUP 响应里取，后续请求自动带上
- CSeq 由 RtspSession 自动分配
- 401 交给应用处理（不做认证）
- 媒体通道只协商不收发（UDP 由应用用 netmech::UdpEndpoint，interleaved 用 session）

用法：
  RtspClient::Config cfg;
  cfg.url = "rtsp://127.0.0.1:8554/live";
  cfg.transport = TransportPreference::udp;   // 或 interleaved
  auto c = RtspClient::create(cfg, my_handler, ec);
  // my_handler->on_playing() 时就可以开始收流了
