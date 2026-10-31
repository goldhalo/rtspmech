#!/bin/bash
# =============================================================================
#  churn.sh —— 断连重连压力测试
# =============================================================================
#  用法：./churn.sh <relay 端口> [每轮拉流数=5] [轮数=10] [单次时长秒=2]
#
#  每轮：
#    1. 同时起 N 个 ffmpeg 拉不同的流
#    2. 等 D 秒
#    3. 全部杀掉（模拟客户端异常断开，不发 TEARDOWN）
#    4. 等 1 秒让服务端清理
#    5. 记录 relay 的 fd / RSS / 线程数
#
#  看什么：
#    * fd 是否回到基准（拉流前）
#    * RSS 是否持续上涨（每轮涨多少）
#    * 如果 fd 一轮涨 N 个不回落 → 会话表泄漏
#
#  比单纯挂 24 小时更容易暴露问题：泄漏的会话表项只有在"连了又断"
#  的时候才会被触碰。
# =============================================================================
set -u

PORT="${1:-8554}"
N="${2:-5}"
ROUNDS="${3:-10}"
DUR="${4:-2}"
OUT=/tmp/churn.csv

command -v ffmpeg > /dev/null || { echo "需要 ffmpeg"; exit 1; }

# 只匹配**进程名**是 rtsp_relay 的，再按端口过滤 cmdline。
# 用 pgrep -f 会连 bash 包装进程一起匹配到（fd 只有 4，样本全错）。
relay_pid() {
    pgrep -x rtsp_relay -a 2>/dev/null \
        | awk -v p="$PORT" '$NF == p { print $1; exit }'
}

PID=$(relay_pid)
[ -z "$PID" ] && { echo "找不到 rtsp_relay $PORT 的进程"; exit 1; }

sample() {
    local rss fds thr
    rss=$(awk '/VmRSS/{print $2}' "/proc/$PID/status" 2>/dev/null || echo 0)
    thr=$(awk '/Threads/{print $2}' "/proc/$PID/status" 2>/dev/null || echo 0)
    fds=$(ls "/proc/$PID/fd" 2>/dev/null | wc -l)
    echo "$rss,$fds,$thr"
}

echo "rss_kb,fds,threads" > "$OUT"
echo "基准（未拉流）："
BASE=$(sample)
echo "$BASE" >> "$OUT"
echo "  RSS ${BASE%%,*} KB, fd $(echo "$BASE" | cut -d, -f2), 线程 $(echo "$BASE" | cut -d, -f3)"
echo

for round in $(seq 1 "$ROUNDS"); do
    # 拉起 N 个客户端（拉到不同的流上）
    pids=()
    for i in $(seq 0 $((N - 1))); do
        idx=$(( (round * N + i) % 100 ))
        ffmpeg -hide_banner -v quiet -rtsp_transport tcp \
               -i "rtsp://127.0.0.1:$PORT/play/$idx" -t "$DUR" -f null - \
               > /dev/null 2>&1 &
        pids+=($!)
    done

    sleep "$((DUR + 1))"

    # 强制杀掉（模拟异常断开，不发 TEARDOWN）
    for p in "${pids[@]}"; do
        kill -9 "$p" 2>/dev/null
    done
    wait 2>/dev/null
    sleep 1

    S=$(sample)
    echo "$S" >> "$OUT"
    printf "第 %2d 轮：RSS %6s KB, fd %4s, 线程 %s\n" \
           "$round" "$(echo "$S" | cut -d, -f1)" \
           "$(echo "$S" | cut -d, -f2)" "$(echo "$S" | cut -d, -f3)"
done

echo
echo "=== 汇总 ==="
python3 - <<'PY'
import csv
rows = [r for r in csv.DictReader(open("/tmp/churn.csv"))
        if r.get("fds") and r.get("rss_kb")]
if len(rows) < 2:
    print("采样点不足")
    raise SystemExit(0)
base = rows[0]
last = rows[-1]
f0, f1 = int(base["fds"]), int(last["fds"])
r0, r1 = int(base["rss_kb"]), int(last["rss_kb"])
t0, t1 = int(base["threads"]), int(last["threads"])
print("fd   : %s → %s  (%+d)" % (f0, f1, f1 - f0))
print("RSS  : %s → %s KB  (%+d KB)" % (r0, r1, r1 - r0))
print("线程 : %s → %s  (%+d)" % (t0, t1, t1 - t0))
n = len(rows) - 1
print("每轮平均：fd %+.2f, RSS %+.1f KB" % ((f1 - f0) / n, (r1 - r0) / n))
print()
if f1 - f0 > n:
    print("!! fd 每轮都在涨，疑似会话表泄漏")
elif r1 - r0 > 200 * n:
    print("!! RSS 每轮涨超过 200KB，疑似内存泄漏")
else:
    print("OK：fd 和 RSS 都回到基准附近")
PY
