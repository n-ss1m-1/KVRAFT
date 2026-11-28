# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
p=Path(__file__).with_name('summarize.py');s=p.read_text()
pos=s.index("lines+=['','## 测量边界")
s=s[:pos]+'''lines+=['','## WAL-off 仅总时间对照（三轮中位数）','','| 批量 | 提交 ms | 含 Flush ms | 含 Flush op/s |','|---|---:|---:|---:|']
for b in [0,1,4,16,64,256,1024]:
 rs=[x for x in rows if x['kind']=='totalonly' and x['batch']==b]
 if len(rs)==3:lines.append(f"| {b} | {med(rs,'submit_ns')/1e6:.3f} | {med(rs,'total_ns')/1e6:.3f} | {16384e9/med(rs,'total_ns'):,.0f} |")
lines+=['','## WAL-on 接口对照额外配对轮','','| 轮次 | 批量 | 提交 ms | p99 µs | max µs |','|---|---:|---:|---:|---:|']
for x in rows:
 if x['kind']=='paired':lines.append(f"| {x['round']} | {x['batch']} | {x['submit_ns']/1e6:.3f} | {x['p99_ns']/1e3:.2f} | {x['max_ns']/1e3:.2f} |")
''' +s[pos:]
s=s.replace('formal、extras、可选 mixed、summarize','formal、extras、paired、totalonly、summarize')
p.write_text(s)
