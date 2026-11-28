# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
from collections import Counter
for p in sorted((O/'runs').glob('*-events.jsonl')):
 name=p.name[:-13];q=p.with_name(name+'.json')
 if not q.exists():continue
 v=json.loads(q.read_text());es=[json.loads(x) for x in p.read_text().splitlines()];known={};consumed=set();ob=fb=0
 for e in sorted(es,key=lambda e:e['published_ns']):
  ins=e['source']+e['target'];out=e['output']
  if e['kind']=='compaction':
   assert e['base_targets']==e['final_targets']==[f['id'] for f in e['target']]
   assert e['finalBytes_ns']==sum(f['bytes'] for f in e['source'])
   assert len(e['source'])==e['baseCount_ns']+e['accepted_ns']
   assert e['baseBytes_ns']==sum(f['bytes'] for f in e['source'][:e['baseCount_ns']])
   assert sum(f['records'] for f in ins)>=sum(f['records'] for f in out)
   if e['accepted_ns']:assert e['finalBytes_ns']<=v['budget']*v['factor']
   for f in ins:assert f['id'] in known and f['id'] not in consumed and known[f['id']]==f;consumed.add(f['id'])
   ob+=sum(f['bytes'] for f in out)
  else:fb+=sum(f['bytes'] for f in out)
  for f in out:assert f['id'] not in known;known[f['id']]=f
 assert set(known)-consumed=={f['file'] for f in v['final']}
 assert ob==v['compaction_output_bytes'] and fb==v['flush_output_bytes']
 ts=[e for e in es if e['kind']=='compaction']
 print(name,'amp',round((ob+fb)/v['logical_encoded_bytes'],4),'tasks',len(ts),'accepted',sum(e['accepted_ns'] for e in ts),Counter(e['stop_reason'] for e in ts),flush=True)
print('ALL FLOW/CREDIT/TARGET CHECKS PASS',flush=True)