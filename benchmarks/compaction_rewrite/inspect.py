# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
from collections import defaultdict
from statistics import median
es=[]
for p in (O/'runs').glob('*-events.jsonl'):
 v=json.loads(p.with_name(p.name.replace('-events.jsonl','.json')).read_text())
 for e in map(json.loads,p.read_text().splitlines()):e['run']=v['name'];e['config']=v['config'];e['work']=v['work'];es.append(e)
for cfg in ['A','B','C']:
 for work in ['unique','update']:
  for level in [0,1]:
   ts=[e for e in es if e['config']==cfg and e['work']==work and e['kind']=='compaction' and e['level']==level]
   if not ts:continue
   def b(e,k):return sum(f['bytes'] for f in e[k])
   ratios=[b(e,'target')/b(e,'source') for e in ts];mx=max(ts,key=lambda e:b(e,'output'))
   print(cfg,work,'L',level,'n',len(ts),'outputMiB',round(sum(b(e,'output') for e in ts)/1048576,2),'ratios p50/max',round(median(ratios),2),round(max(ratios),2),'max io MiB',[round(b(mx,k)/1048576,2) for k in ['source','target','output']])