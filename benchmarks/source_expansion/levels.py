# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
from collections import defaultdict
from statistics import median
import csv
rows=list(csv.DictReader((O/'tasks.csv').open()));g=defaultdict(list)
for x in rows:
 if x['run'].startswith('overhead'):continue
 g[x['scale'],int(x['budget']),int(x['factor']),x['work'],int(x['level'])].append(x)
lines=['# 按层机制与任务分布','', '|规模|预算KiB|倍率|负载|源层|样本任务|基础源KiB中位|最终源KiB中位|接受文件|新增目标停止|输出MiB|清除旧记录|总输入p50/p95/max MiB|','|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---|']
for k,xs in sorted(g.items()):
 vals=sorted(int(x['total_input_bytes']) for x in xs);q=vals[int(.95*(len(vals)-1))]
 lines.append('|'+ '|'.join(map(str,[k[0],k[1]//1024,*k[2:],len(xs),round(median(int(x['base_bytes']) for x in xs)/1024,2),round(median(int(x['final_source_bytes']) for x in xs)/1024,2),sum(int(x['accepted']) for x in xs),sum(x['stop']=='new_target' for x in xs),round(sum(int(x['output_bytes']) for x in xs)/1048576,2),sum(int(x['dropped_versions']) for x in xs),str([round(y/1048576,2) for y in [median(vals),q,max(vals)]])]))+'|')
lines+=['','各组机制轮数不同，输出MiB和清理数量是采集任务合计，不能直接作为组间倍数；每轮汇总见mechanism-runs.csv。停止原因每任务一个，前面可已接受若干文件。倍率1的bytes不是“扩展策略尝试成功”。','', '## 源预算/文件大小证据','']
for p in sorted((O/'runs').glob('*-events.jsonl')):
 if not p.name.startswith(('pilot','cpilot')):continue
 q=p.with_name(p.name.replace('-events.jsonl','.json'))
 if not q.exists():continue
 es=[json.loads(x) for x in p.read_text().splitlines()];fs=[f['bytes'] for e in es if e['kind']=='flush' for f in e['output']]
 lines.append(f'- {p.stem}: Flush {len(fs)}次，文件bytes min/median/max={min(fs)}/{median(fs)}/{max(fs)}。')
# Independent overhead, do not add to main group medians.
lines+=['','## 详细机制开销配对','', '|轮次|种类|请求op/s|请求至收敛ms|RSS MiB|','|---:|---|---:|---:|---:|']
for p in sorted((O/'runs').glob('overhead*.json')):
 if p.name.endswith('backpressure.json'):continue
 v=json.loads(p.read_text())
 lines.append('|'+ '|'.join(map(str,[v['name'],v['kind'],round(v['writes']['logical_ops_s']),round((v['converged_ns']-v['begin_ns'])/1e6,3),round(v['rss_kib']/1024,2)]))+'|')
(O/'LEVELS.md').write_text('\n'.join(lines)+'\n');print('level details ready',flush=True)# Aggregate physical per-layer output across independent runs, explicitly normalize per run.
vs={p.stem:json.loads(p.read_text()) for p in (O/'runs').glob('*.json') if not p.name.endswith('-backpressure.json')}
by=defaultdict(list)
for p in (O/'runs').glob('*-events.jsonl'):
 if p.name.startswith('overhead'):continue
 name=p.name[:-13]
 if name not in vs:continue
 v=vs[name];es=[json.loads(x) for x in p.read_text().splitlines()];out=[0]*7;counts=[0]*7;drop=[0]*7
 for e in es:
  l=e['level']+1 if e['kind']=='compaction' else 0;out[l]+=sum(f['bytes'] for f in e['output']);counts[l]+=1
  if e['kind']=='compaction':drop[l]+=sum(f['records'] for f in e['source']+e['target'])-sum(f['records'] for f in e['output'])
 by[v['scale'],v['budget'],v['factor'],v['work']].append({'bytes':out,'count':counts,'drop':drop,'denom':v['logical_encoded_bytes']})
ls=['# 每层输出贡献（独立机制各轮中位）','','|规模|预算KiB|倍率|负载|轮数|Flush贡献|L1贡献|L2贡献|L0→L1任务|L1→L2任务|清除记录中位|','|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|']
for k,xs in sorted(by.items()):
 vals=[round(median(x['bytes'][l]/x['denom'] for x in xs),4) for l in [0,1,2]]
 ls.append('|'+ '|'.join(map(str,[k[0],k[1]//1024,*k[2:],len(xs),*vals,median(x['count'][1] for x in xs),median(x['count'][2] for x in xs),median(sum(x['drop']) for x in xs)]))+'|')
(O/'AMPLIFICATION.md').write_text('\n'.join(ls)+'\n')