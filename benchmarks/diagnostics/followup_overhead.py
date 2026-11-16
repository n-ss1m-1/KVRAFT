from pathlib import Path
import json,collections,statistics
p=Path('/home/truth/Code/my_kvraft/benchmarks/results/diagnostics-20261116-followup');rows=[json.loads(x) for x in (p/'runs.jsonl').read_text().splitlines()]
g=collections.defaultdict(dict)
for r in rows:
 if not r['name'].startswith('overhead'):continue
 rep=r['name'].split('-')[1];kind=r['name'].split('-')[2]
 for v in (r['results'] if r['query'] else [dict(name='put',ops_per_second=r['request_ops'],p99_us=r['p99_us'])]):g[(kind,v['name'])].setdefault(rep,{})[r['on']]=v
for k,vs in g.items():
 pairs=[v for v in vs.values() if len(v)==2]
 if pairs:print(k,'throughput%', [round(100*(v['on']['ops_per_second']/v['off']['ops_per_second']-1),2) for v in pairs],'p99%', [round(100*(v['on']['p99_us']/v['off']['p99_us']-1),2) for v in pairs])
