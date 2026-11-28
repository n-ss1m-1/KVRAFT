# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
import csv
from collections import defaultdict
from statistics import median
rows=list(csv.DictReader((O/'tasks.csv').open()));g=defaultdict(list)
for x in rows:
 if x['kind']=='compaction':g[x['run'].split('-')[-2:] if False else (x['run'],x['source_level'])].append(x)
rs=[]
for (run,level),xs in g.items():
 v=json.loads((O/'runs'/(run+'.json')).read_text())
 for f in ['source_bytes','target_bytes','output_bytes','ratio_target_source']:
  a=sorted(float(x[f]) for x in xs);q=lambda p:a[min(len(a)-1,int(p*(len(a)-1)))]
  rs.append(dict(run=run,config=v['config'],work=v['work'],source_level=level,metric=f,count=len(a),min=a[0],p50=median(a),p95=q(.95),max=a[-1]))
with (O/'task-distributions.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=list(rs[0]));w.writeheader();w.writerows(rs)
# Formal paired directions and retained maxima, no removal or trimming.
vs=[json.loads(p.read_text()) for p in (O/'runs').glob('f*.json')]
lines=['# 波动与逐轮尾延迟','','|负载|轮|A请求op/s|B请求op/s|A总收敛ms|B总收敛ms|A max ms|B max ms|','|---|---:|---:|---:|---:|---:|---:|---:|']
for work in ['unique','update']:
 for r in range(6):
  a=next(v for v in vs if v['name']==f'f{r}-{work}-A');b=next(v for v in vs if v['name']==f'f{r}-{work}-B')
  lines.append('|'+ '|'.join(map(str,[work,r,round(a['writes']['logical_ops_s']),round(b['writes']['logical_ops_s']),round((a['converged_ns']-a['begin_ns'])/1e6,2),round((b['converged_ns']-b['begin_ns'])/1e6,2),round(a['writes']['max_ns']/1e6,3),round(b['writes']['max_ns']/1e6,3)]))+'|')
lines+=['','## C更新负载逐轮最大批次延迟','']
for v in sorted(vs,key=lambda x:x['name']):
 if v['config']=='C' and v['auto'] and v['work']=='update':lines.append(f'- {v["name"]}: max={v["writes"]["max_ns"]/1e6:.3f} ms，request={v["writes"]["logical_ops_s"]:.0f} op/s。')
(O/'VARIABILITY.md').write_text('\n'.join(lines)+'\n');print('\n'.join(lines),flush=True)