test: 加长时间运行监控脚本 + 22 分钟观测结果

  example/soak.sh         每 N 秒采 RSS / fd / 线程数 → /tmp/soak.csv
  example/soak_report.py  把 CSV 汇总成趋势表

观测条件：100 路推流（50 TCP + 50 UDP）+ 中转分发，单机回环。

22 分钟结果：

  rtsp_relay（100 个推流连接 + 100 个 UDP 端口）
    fd   167 → 170，第 6 分钟之后**纹丝不动**
    线程 9 固定（4 io + 4 worker + 1 main）
    RSS  6684 → 7056 KB，但第 6 分钟后 16 分钟只涨 20 KB

  rtsp_pusher（100 路客户端）
    fd   166 → 166，完全稳定
    线程 5 固定
    RSS  7772 → 7940 KB（同样在启动期后进平台）

  吞吐 2770 万包 / 1330 秒 = 20,835 包/秒

结论：22 分钟内没有泄漏迹象。启动期的 RSS 上涨是连接缓冲和内存池
首次扩容，第 6 分钟后就进入平台期 —— 按那一小段的斜率外推 24h 会
误判成"24 小时涨 100MB"。

要确认 24 小时真的稳，需要让它一直跑：

  ./build-linux/bin/rtsp_relay 8554 &
  ./build-linux/bin/rtsp_pusher rtsp://127.0.0.1:8554 file.h264 100 --mixed &
  ./example/soak.sh 60 &
  # 隔天：python3 example/soak_report.py
