feat: RtspClient 支持推流模式（ANNOUNCE → SETUP → RECORD）

之前只有拉流（DESCRIBE → PLAY）。现在两种模式：

  pull：OPTIONS → DESCRIBE → SETUP → PLAY    （从服务端取流）
  push：OPTIONS → ANNOUNCE → SETUP → RECORD   （往服务端送流）

推流模式下：
- 客户端提供 SDP（cfg.sdp），ANNOUNCE 时带给服务端
- track 列表从自己的 SDP 里取（没有 DESCRIBE 响应可用）
- SETUP 响应里记下服务端的收流端口（server_udp_port / server_rtcp_port）
- RECORD 成功后回调 on_recording()，可以开始发 RTP

媒体收发仍然不在库里：
  UDP  → 应用用 netmech::UdpEndpoint 往 host:server_udp_port() 发
  TCP  → 应用用 session()->send_interleaved(channel, ...) 发

每路推流必须用不同的 URL（服务端靠它区分源，分发时靠它定位）。
