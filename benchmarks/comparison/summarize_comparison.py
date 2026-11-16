import collections
import json
from pathlib import Path
import statistics as st
root=Path(__file__).resolve().parents[2]
out=root/'benchmarks/results/background-20261116'
meta=json.loads((out/'metadata.json').read_text())
rows=[json.loads(s) for s in (out/'results.jsonl').read_text().splitlines()]
assert len(rows)==len(meta['schedule'])==48
for i,r in enumerate(rows):
 assert r['index']==i and all(r[k]==v for k,v in meta['schedule'][i].items())
 assert r['verified'] and r['sstables_after_close']>=3
 assert r['p50_us']<=r['p95_us']<=r['p99_us']<=r['max_us']
 assert r['total_with_flush_seconds']>=r['request_seconds']
 assert r['gets']+r['puts']+r['deletes']==5000
 assert r['verified_keys_twice']==5000
 raw=list(__import__('csv').DictReader((out/f'{i:03d}-{r["engine"]}-{r["mode"]}-w{r["wal"]}-t{r["threshold"]}.latency.csv').open()))
 assert len(raw)==5000
 g=collections.defaultdict(list)
for r in rows:g[(r['mode'],r['wal'],r['threshold'],r['engine'])].append(r)
text=['# 同步刷盘与后台刷盘对比','','5000 ops，16 B key / 128 B value，seed 20261115，1 个前台线程，每组 3 次。Release / GCC 11.4 / WSL2 ext4。新版本另有 1 个引擎后台线程。','','下表各项为三轮中位数，最大停顿为三轮最大值；延迟只覆盖请求，不含最终 Flush。总耗时为请求开始到最终 Flush 返回。','','| workload | WAL | 阈值 KiB | 引擎 | 请求 op/s（最小–最大） | 请求 s | Flush s | 总 s | 总 op/s | p50 µs | p95 µs | p99 µs | 最大 µs | 峰值 KiB | SST 数范围 |','| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |']
for (mode,wal,limit,engine),rs in sorted(g.items()):
 assert len(rs)==3 and len({r['repeat'] for r in rs})==3
 def med(k):return st.median(r[k] for r in rs)
 rates=[r['request_ops_per_second'] for r in rs];counts=[r['sstables_after_close'] for r in rs]
 text.append(f'| {mode} | {wal} | {limit//1024} | {engine} | {med("request_ops_per_second"):,.0f} ({min(rates):,.0f}–{max(rates):,.0f}) | {med("request_seconds"):.4f} | {med("final_flush_seconds"):.4f} | {med("total_with_flush_seconds"):.4f} | {med("total_ops_per_second"):,.0f} | {med("p50_us"):.2f} | {med("p95_us"):.2f} | {med("p99_us"):.2f} | {max(r["max_us"] for r in rs):.2f} | {med("peak_rss_through_flush_kib"):,.0f} | {min(counts)}–{max(counts)} |')
text+=['','## 吞吐变化（background / sync − 1）','','| workload | WAL | 阈值 KiB | 请求吞吐 | 含 Flush 吞吐 |','| --- | --- | ---: | ---: | ---: |']
for mode,wal,limit in sorted({(r['mode'],r['wal'],r['threshold']) for r in rows}):
 a=g[(mode,wal,limit,'sync')];b=g[(mode,wal,limit,'background')]
 def ratio(k):return (st.median(r[k] for r in b)/st.median(r[k] for r in a)-1)*100
 text.append(f'| {mode} | {wal} | {limit//1024} | {ratio("request_ops_per_second"):+.1f}% | {ratio("total_ops_per_second"):+.1f}% |')
text+=['','全部 48 场景通过请求读取 oracle、Flush 后逐 key 校验及关闭后禁用 WAL 重开校验。每次原始请求延迟均保留在 latency.csv。','','限制：整次升级还包括分段 WAL、锁与查询路径变更，不能将差异全部归于后台刷盘；无独立背压计时接口，最大请求延迟包含背压、锁与 I/O，不能拆分。峰值为进程 VmHWM，含 fixture 与 harness，不是引擎独占内存。未清 OS 缓存；跳表随机高度未固定。旧历史报告使用不同计时口径和 RSS 统计方式，本报告采用历史源码重跑，不直接计算历史数字的百分比。','','总时间不含 DB 初次打开、混合负载装载、最终验证、关闭以及重开；新 Flush 保证目标序列持久化，不保证旧 WAL 清理完成，关闭时 join 清理成本不在总时间内。最终磁盘大小在关闭和验证完成后统计。','']
(out/'SUMMARY.md').write_text('\n'.join(text))
print(f'Validated {len(rows)} runs / {sum(r["operations"] for r in rows)} raw latency records; summary written')
