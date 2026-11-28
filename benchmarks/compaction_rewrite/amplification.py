# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
from collections import defaultdict
from statistics import median
ss=json.loads((O/'mechanism-summary.json').read_text());gg=defaultdict(list)
for s in ss:gg[s['config'],s['work']].append(s)
lines=['# 输出放大贡献与最大任务','', '|配置|负载|Flush/L0贡献|L1输出贡献|L2输出贡献|总放大中位|零版本清理Compaction输出占比|','|---|---|---:|---:|---:|---:|---:|']
for k,g in sorted(gg.items()):
 vals=[median(x['output_by_level'].get(str(l),0)/x['logical_encoded_bytes'] for x in g) for l in [0,1,2]]
 z=median(x['zero_drop_output_bytes']/(x['sst_output_bytes']-x['output_by_level'].get('0',0)) for x in g)
 lines.append('|'+ '|'.join(map(str,[*k,*[round(v,4) for v in vals],round(median(x['amp'] for x in g),4),round(z*100,2)]))+'|')
lines+=['','分母为成功逻辑EncodedEntrySize总和，贡献不是输出层最终空间。下面列主机制轮按输出量排名前12任务，完整分布见tasks.csv、每文件见原事件。','', '|轮次|任务|源层|源MiB|目标MiB|输出MiB|丢弃旧记录|','|---|---:|---:|---:|---:|---:|---:|']
import csv
rs=[r for r in csv.DictReader((O/'tasks.csv').open()) if r['kind']=='compaction']
for r in sorted(rs,key=lambda r:int(r['output_bytes']),reverse=True)[:12]:lines.append('|'+ '|'.join(map(str,[r['run'],r['task'],r['source_level'],*[round(int(r[f])/1048576,2) for f in ['source_bytes','target_bytes','output_bytes']],r['dropped_versions']]))+'|')
(O/'AMPLIFICATION.md').write_text('\n'.join(lines)+'\n');print('\n'.join(lines),flush=True)