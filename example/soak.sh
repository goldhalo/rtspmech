#!/bin/bash
# =============================================================================
#  soak.sh —— 长时间运行监控
# =============================================================================
#  用法：./soak.sh [采样间隔秒=30]
#
#  每 N 秒采一次 rtsp_relay / rtsp_pusher 的：
#    RSS（KB）、打开的文件描述符数、线程数
#
#  输出：/tmp/soak.csv（追加）
#    ts,iso,pid,proc,rss_kb,fds,threads
#
#  看趋势：
#    column -s, -t /tmp/soak.csv | less
# =============================================================================
set -u
INTERVAL="${1:-30}"
OUT=/tmp/soak.csv

if [ ! -f "$OUT" ]; then
    echo "ts,iso,pid,proc,rss_kb,fds,threads" > "$OUT"
fi

echo "采样中（每 ${INTERVAL}s）→ $OUT"
echo "按 Ctrl-C 停止"

while true; do
    total_rss=0
    total_fds=0
    for pid in $(pgrep -f 'build-linux/bin/rtsp_(relay|pusher)' 2>/dev/null); do
        [ -d "/proc/$pid" ] || continue
        rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
        thr=$(awk '/Threads/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
        fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
        name=$(awk '{print $2}' "/proc/$pid/comm" 2>/dev/null | tr -d '()')
        [ -z "$name" ] && name=$(cat "/proc/$pid/comm" 2>/dev/null)
        echo "$(date +%s),$(date -Iseconds),$pid,$name,$rss,$fds,$thr" >> "$OUT"
        total_rss=$((total_rss + rss))
        total_fds=$((total_fds + fds))
    done
    echo "[$(date +%H:%M:%S)] 合计 RSS $((total_rss / 1024)) MB，fd $total_fds"
    sleep "$INTERVAL"
done
