# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,csv,collections,statistics as st
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/block-cache-acceptance-20261119';rows=[json.loads(x) for x in (o/'runs.jsonl').read_text().splitlines()]
formal=[v for v in rows if v['name'].startswith('formal')];mechanism=[v for v in rows if v['name'].startswith('mechanism')]
for name,data in [('formal.csv',formal),('mechanism.csv',mechanism)]:
 if data:
  with (o/name).open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=[k for k in data[0] if k!='command']);w.writeheader();w.writerows({k:v for k,v in x.items() if k!='command'} for x in data)
g=collections.defaultdict(list)
for v in formal:g[(v['tables'],v['path'],v['kind'],v['capacity_label'],v['pass'])].append(v)
lines=['# 块缓存验收结果','','正式数据：Release C++17、查表 CRC、诊断关闭；每场景独立进程/空缓存，计时前独立无缓存 Reader 预热全部文件块。每组 3 轮、每个 pass 8192 请求，种子 20261115。pass 0=填充，pass 1=稳定访问。','','| tables | path | case | capacity | pass | op/s median [min,max] | p50 us | p95 us | p99 us | max us | Usage B | VmHWM KiB |','| ---: | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
for k,vs in sorted(g.items()):
 def med(f):return st.median(v[f] for v in vs)
 lines.append('| '+' | '.join(map(str,k))+f' | {med("ops"):.0f} [{min(v["ops"] for v in vs):.0f},{max(v["ops"] for v in vs):.0f}] | {med("p50_us"):.3f} | {med("p95_us"):.3f} | {med("p99_us"):.3f} | {max(v["max_us"] for v in vs):.3f} | {med("usage"):.0f} | {med("rss_kib"):.0f} |')
lines+=['','## 稳定轮吞吐相对容量 0','','| tables | path | kind | full ratio | small ratio |','| ---: | --- | --- | ---: | ---: |']
for nt,path,kind in sorted({(v['tables'],v['path'],v['kind']) for v in formal}):
 def ops(label):return st.median(v['ops'] for v in g[(nt,path,kind,label,1)])
 if all((nt,path,kind,x,1) in g for x in ['disabled','full','small']):lines.append(f'| {nt} | {path} | {kind} | {ops("full")/ops("disabled"):.3f}× | {ops("small")/ops("disabled"):.3f}× |')
lines+=['','机制轮独立程序有 Get/Insert/pread 链接包装，仅机制 CSV 可解释命中率，不能将其吞吐与正式性能混用。正式缓存统计字段为 0 表示未启用观测，不是命中率为 0。Usage 来自实际共享缓存，容量以正文计费，不含 CRC、节点、Reader 对象和分配器额外内存。VmHWM 为进程累计峰值，含预热、trace、结果数组及 allocator 保留内存，不能视为引擎独占内存。','']
(o/'SUMMARY.md').write_text('\n'.join(lines))
print('formal samples',len(formal),'mechanism samples',len(mechanism))
print('\n'.join(lines[lines.index('## 稳定轮吞吐相对容量 0'):]))
