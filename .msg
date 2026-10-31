feat: RTSP 推流 + 中转分发（100 路混合实测通过）

新增两个示例和一个库级修复：

  rtsp_pusher  推流客户端，N 路循环推 H264（每路独立 URL /push/<id>）
  rtsp_relay   中转服务器：收流（ANNOUNCE/RECORD）+ 分发（DESCRIBE/PLAY）
               外部 ffmpeg 随时可以拉任意一路播放

库级修复（不是示例补丁）：

  RequestBuilder::body() 现在自动补 Content-Length。
  之前只有 ResponseBuilder 会补，ANNOUNCE 带 SDP 时接收端的 framer
  无法确定 body 长度，把 SDP 当成下一条消息解析失败 → 断链。

实测结果（100 路 = 50 TCP + 50 UDP）：

  100 路全部 RECORD 成功
  15 秒发出 313,956 个 RTP 包（约 20,900 包/秒）
  拉第 0 路（TCP）  → ffmpeg 解码零错误
  拉第 60 路（UDP） → 能解码（中途加入会有 Missing reference picture，
                     下一个 IDR 恢复，属正常现象）

调试中踩到并修掉的问题（都写进了注释）：

1. Content-Base 硬编码 127.0.0.1，丢了端口 → SETUP 的 URL 变成
   rtsp://127.0.0.1/0/trackID=0，找不到流。

2. stream_id() 遇到末尾斜杠返回空串 → PLAY 的 URI 是 `.../play/0/`，
   rfind('/') 指到最后一个字符。

3. SPS/PPS 只在文件开头发一次 → 中途加入的 ffmpeg 报
   "non-existing PPS 0"。改成每帧都带（每帧多约 30 字节，6kbps）。

4. netmech::UdpEndpoint::send_to 是异步的，ConstBuf 的 owner 为空时
   库不持有数据。推流端和 relay 转发都踩了这个坑：栈上/接收缓冲被
   下一帧覆盖，对端收到的是垃圾。改成 shared_ptr 持有（多个 UDP
   订阅者共享一份拷贝）。

5. UDP 服务端端口固定从 50000 分配 → Address already in use。
   改成失败就试下一个，最多 20 次。
