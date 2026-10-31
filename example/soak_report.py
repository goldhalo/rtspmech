#!/usr/bin/env python3
"""把 soak.sh 的 CSV 汇总成趋势表。

用法：python3 soak_report.py [/tmp/soak.csv]
"""
import csv
import collections
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/soak.csv"

by = collections.defaultdict(list)
for r in csv.DictReader(open(path)):
    if r["proc"].startswith("rtsp_"):
        by[r["proc"]].append(
            (int(r["ts"]), int(r["rss_kb"]), int(r["fds"]), int(r["threads"]))
        )

if not by:
    print("没有 rtsp_* 的采样点")
    sys.exit(0)

for name in sorted(by):
    pts = sorted(by[name])
    t0 = pts[0][0]
    print("=== " + name + "（" + str(len(pts)) + " 个采样点）===")
    print("  分钟   RSS(KB)    fd  线程")
    step = max(1, len(pts) // 10)
    for ts, rss, fds, thr in pts[::step] + [pts[-1]]:
        print("%6d %9d %5d %5d" % ((ts - t0) // 60, rss, fds, thr))

    mins = max((pts[-1][0] - pts[0][0]) / 60.0, 0.1)
    drss = pts[-1][1] - pts[0][1]
    dfd = pts[-1][2] - pts[0][2]
    print("  RSS : %d → %d KB  (%+.1f KB/分)" % (pts[0][1], pts[-1][1], drss / mins))
    print("  fd  : %d → %d      (%+.2f/分)" % (pts[0][2], pts[-1][2], dfd / mins))
    print("  线程: %d → %d" % (pts[0][3], pts[-1][3]))
    print()
