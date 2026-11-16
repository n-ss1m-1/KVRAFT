#!/usr/bin/env python3
"""Summarize independent repetitions without pooling per-run percentiles."""
import argparse
import collections
import json
from pathlib import Path
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("results", type=Path)
args = parser.parse_args()
rows = [json.loads(line) for line in (args.results / "results.jsonl").read_text().splitlines()]
groups = collections.defaultdict(list)
for row in rows:
    group = (row["mode"], row["phase"], row["wal"], row["configured_operations"], row["configured_tables"], row["memtable_limit"])
    groups[group].append(row)
text = ["# 同步刷盘首轮性能基线", "", "参数、源码和环境见 metadata.json；原始逐轮结果见 results.csv / results.jsonl。", "",
        "各指标为独立重复轮次的中位数；p99 是各轮 p99 的中位数，不是合并样本的 p99。括号内为吞吐最小值–最大值。", "",
        "| 场景 / phase | WAL | N | 初始表数 | 阈值 B | 重复 | API op/s（范围） | p50 µs | p95 µs | p99 µs | 最大 µs¹ | CPU %² | 峰值 RSS KiB³ | 结束文件 B |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
for group, samples in sorted(groups.items()):
    mode, phase, wal, n, count, limit = group
    def med(field):
        return statistics.median(x[field] for x in samples)
    ops = [x["ops_per_second"] for x in samples]
    text.append(f"| {mode} / {phase} | {'on' if wal else 'off'} | {n} | {count} | {'disabled' if limit == (1 << 63) - 1 else limit} | {len(samples)} | {statistics.median(ops):,.0f} ({min(ops):,.0f}–{max(ops):,.0f}) | {med('p50_us'):,.2f} | {med('p95_us'):,.2f} | {med('p99_us'):,.2f} | {max(x['max_us'] for x in samples):,.2f} | {med('cpu_percent_one_core'):,.1f} | {med('process_peak_rss_kib'):,.0f} | {med('disk_bytes_end'):,.0f} |")
text.extend(["", "¹ 最大延迟取全部重复轮次的最大值。", "² CPU 为整个 phase 观察区间；mixed 分类和触发刷盘子集不代表独占 CPU。", "³ RSS 为进程累计高水位，含 fixture 与 harness。文件字节数是结束时逻辑大小。", "", "所有读取都未清 OS 缓存；first_pass 不能视为磁盘冷读。API 吞吐排除 harness 时间，墙钟吞吐另见原始结果。", "Open 每轮 20 次、手动 Flush 每轮 20 次，尾部分位数样本少；其他场景以 operations 字段为准。", "此结果来自 WSL2 ext4 虚拟盘，小规模开发负载；不能据此推断生产容量或宿主物理盘性能。", ""])
(args.results / "SUMMARY.md").write_text("\n".join(text))
print(f"Summarized {len(rows)} rows in {len(groups)} groups")
