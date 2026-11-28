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
 if 'name' in v:vs.append(v)
lines=['# 逐轮值、同额度对照与查询','', '同额度是预算乘倍率的理论额度，不是实际任务硬限（首文件可能超限，目标不受限）。','']
g=defaultdict(list)
for v in vs:
 if v['name'].startswith(('s1','s2','wal')):g[v['scale'],v['wal'],v['work'],v['budget'],v['factor']].append(v)
for scale in ['A','C']:
 for wal in [0,1]:
  for work in ['unique','update']:
   for left,right in [((262144,4),(1048576,1)),((1048576,4),(4194304,1)),((1048576,1),(1048576,2)),((1048576,2),(1048576,4)),((4194304,1),(4194304,2)),((4194304,2),(4194304,4))]:
    a=g.get((scale,wal,work,*left));b=g.get((scale,wal,work,*right))
    if not a or not b:continue
    n=min(len(a),len(b));a=sorted(a,key=lambda v:v['name']);b=sorted(b,key=lambda v:v['name'])
    lines+=['',f'## {scale} WAL={wal} {work}: {left[0]//1024}KiB×{left[1]} → {right[0]//1024}KiB×{right[1]}', '', '|配对轮|左输出放大|右输出放大|左请求op/s|右请求op/s|左总收敛ms|右总收敛ms|左max ms|右max ms|','|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
    for i,(x,y) in enumerate(zip(a,b)):
     amp=lambda v:(v['flush_output_bytes']+v['compaction_output_bytes'])/v['logical_encoded_bytes']
     lines.append('|'+ '|'.join(map(str,[i,round(amp(x),4),round(amp(y),4),round(x['writes']['logical_ops_s']),round(y['writes']['logical_ops_s']),round((x['converged_ns']-x['begin_ns'])/1e6,2),round((y['converged_ns']-y['begin_ns'])/1e6,2),round(x['writes']['max_ns']/1e6,3),round(y['writes']['max_ns']/1e6,3)]))+'|')
lines+=['','## 收敛后查询：稳态三轮中位','', '|规模|预算KiB|倍率|写入负载|查询类型|缓存|轮数|op/s|p95 us|p99 us|max us|Usage B|容量 B|','|---|---:|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|']
qg=defaultdict(list)
for v in vs:
 if v['mode']=='query':qg[v['scale'],v['budget'],v['factor'],v.get('fixture_work'),v['work'],v['cache_capacity']>0].append(v)
for k,gp in sorted(qg.items()):
 m=lambda f:median(f(v) for v in gp)
 lines.append('|'+ '|'.join(map(str,[k[0],k[1]//1024,*k[2:],len(gp),round(m(lambda v:v['steady']['api_s'])),*[round(m(lambda v:v['steady'][q+'_ns'])/1000,3) for q in ['p95','p99','max']],round(m(lambda v:v['usage_steady'])),round(m(lambda v:v['cache_capacity']))]))+'|')
(O/'COMPARISONS.md').write_text('\n'.join(lines)+'\n')
# Per-run mechanism totals and flush file-size distribution.
rows=[]
for p in (O/'runs').glob('*-events.jsonl'):
 name=p.name[:-13];q=p.with_name(name+'.json')
 if not q.exists():continue
 v=json.loads(q.read_text());es=[json.loads(x) for x in p.read_text().splitlines()];tasks=[e for e in es if e['kind']=='compaction'];fs=[f['bytes'] for e in es if e['kind']=='flush' for f in e['output']];bp=json.loads(p.with_name(name+'-backpressure.json').read_text())
 rows.append(dict(run=name,scale=v['scale'],budget=v['budget'],factor=v['factor'],work=v['work'],tasks=len(tasks),level0=sum(e['level']==0 for e in tasks),level1=sum(e['level']==1 for e in tasks),accepted=sum(e['accepted_ns'] for e in tasks),expanded_tasks=sum(e['accepted_ns']>0 for e in tasks),base_source_bytes=sum(e['baseBytes_ns'] for e in tasks),final_source_bytes=sum(e['finalBytes_ns'] for e in tasks),backpressure_count=bp['count'],backpressure_ns=bp['ns'],flush_file_min=min(fs),flush_file_p50=median(fs),flush_file_max=max(fs),compaction_output_bytes=v['compaction_output_bytes']))
if rows:
 with (O/'mechanism-runs.csv').open('w') as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
print('comparison tables ready',flush=True)