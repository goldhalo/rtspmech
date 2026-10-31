fix: 关闭会话时的自析构崩溃 + 加断连重连测试

上一版加了 on_close 清理后，churn 测试直接段错误。根因是**关闭过程中
对象把自己析构掉了**：

  Impl::close() 是 Impl 的方法
    → 回调 on_close
    → 应用（relay）里把 session 从所有表里移除，引用计数归零
    → RtspSession 析构 → Impl（它的成员）跟着析构
    → close() 还在 Impl 的栈上跑，返回时访问已释放内存 → SIGSEGV

修法是两层保活：

  RtspSession 继承 enable_shared_from_this
  Impl::close() 里 weak_from_this().lock() 抓一份强引用
  Impl 也继承 enable_shared_from_this，同样在 close() 里保活自己

  这样整个关闭过程中对象一定活着，清理动作可以放心做。

新增 example/churn.sh —— 断连重连压力测试：
  每轮拉起 N 个 ffmpeg 拉流，D 秒后 kill -9（模拟异常断开，不发
  TEARDOWN），再采样 relay 的 fd / RSS / 线程数。

  这比单纯挂 24 小时更容易暴露问题：泄漏的会话表项只有在"连了又断"
  的时候才会被触碰。之前 22 分钟的 soak（100 路一直连着）就没发现
  这个 bug。

实测（10 路推流，15 轮 × 5 客户端 = 75 次连断）：

  基准   RSS 5468 KB, fd 32, 线程 9
  15 轮后 RSS 5492 KB, fd 32, 线程 9

  fd   : 32 → 32   (+0)          ← 零泄漏
  RSS  : +24 KB     (1.6 KB/轮)
  线程 : 9 → 9      (+0)

顺带修了脚本的两个坑：pgrep -f 会连 bash 包装进程一起匹配（fd 只有 4，
样本全错），改用 pgrep -x 按进程名 + 端口过滤；以及 CSV 表头多了一列
port 导致汇总脚本崩。
