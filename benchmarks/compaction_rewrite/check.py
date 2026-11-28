# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
from collections import Counter
for cfg in ['A','B','C']:
 for work in ['unique','update']:
  name='pilot-'+cfg+'-'+work;p=O/'runs'/name
  es=[json.loads(x) for x in p.with_name(name+'-events.jsonl').read_text().splitlines()];v=json.loads(p.with_suffix('.json').read_text());known={};consumed=set();out=0;nr=0
  for e in sorted(es,key=lambda e:e['published_ns']):
   if e['kind']=='compaction':
    ins=e['source']+e['target'];assert all(f['id'] in known and f['id'] not in consumed for f in ins)
    for f in ins:assert known[f['id']]==f;consumed.add(f['id'])
    assert sum(f['records'] for f in ins)>=sum(f['records'] for f in e['output'])
   for f in e['output']:
    assert f['id'] not in known;known[f['id']]=f;out+=f['bytes']
  final={f['file'] for f in v['final']};assert set(known)-consumed==final
  print(name,Counter(e['level'] for e in es if e['kind']=='compaction'),'flush',sum(e['kind']=='flush' for e in es),'amp',out/v['logical_encoded_bytes'],flush=True)
print('FLOW CHECK PASS')