# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
from statistics import median
from collections import defaultdict
import csv
vs=[]
for p in (O/'runs').glob('*.json'):
 if p.name.endswith('-backpressure.json'):continue
 v=json.loads(p.read_text())
 if 'name' in v and v.get('verified'):vs.append(v)
g=defaultdict(list)
for v in vs:
 if v['name'].startswith(('s1','s2','wal')):
  g[v['scale'],v['wal'],v['budget'],v['factor'],v['work']].append(v)
lines=['# 恢复前后分层与逐轮指标','','2026-11-28 续跑：历史 3 份空 JSON 和 1 个无完整结果目录不作为有效轮。原件保留在历史目录，缺失轮在续跑目录补齐。','历史完整记录复制到当前 runs；通过 commands.jsonl（本目录仅续跑命令）区分。不要把恢复前后环境变化归为参数因果。','','|规模/WAL/预算/倍率/负载|阶段|轮数|请求op/s中位|min–max|总收敛s中位|输出放大中位|','|---|---|---:|---:|---|---:|---:|']
new=set()
for x in (O/'commands.jsonl').read_text().splitlines():
 a=json.loads(x)['argv'];new.add(Path(a[3]).name)
for k,arr in sorted(g.items()):
 for label,sub in [('全部',arr),('故障前',[v for v in arr if v['name'] not in new]),('恢复后',[v for v in arr if v['name'] in new])]:
  if not sub:continue
  ops=[v['writes']['logical_ops_s'] for v in sub]
  lines.append('|'+ '|'.join(map(str,[k,label,len(sub),round(median(ops)),str([round(min(ops)),round(max(ops))]),round(median((v['converged_ns']-v['begin_ns'])/1e9 for v in sub),3),round(median((v['flush_output_bytes']+v['compaction_output_bytes'])/v['logical_encoded_bytes'] for v in sub),4)]))+'|')
(O/'RECOVERY-STRATA.md').write_text('\n'.join(lines)+'\n')
# Validate all raw latency rows, including historical files, before final acceptance.
bad=[]
for v in vs:
 for label in ['writes','first','steady']:
  if label not in v:continue
  f=O/'runs'/(v['name']+'-'+label+'.csv')
  n=sum(1 for _ in csv.DictReader(f.open())) if f.exists() else -1
  if n!=v[label]['count']:bad.append([v['name'],label,n,v[label]['count']])
(O/'raw-integrity.json').write_text(json.dumps({'runs':len(vs),'invalid':bad},indent=2))
assert not bad,bad
print('RAW INTEGRITY PASS',len(vs))