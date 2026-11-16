from pathlib import Path
import json,collections,statistics
p=Path('/home/truth/Code/my_kvraft/benchmarks/results/diagnostics-20261116-first');rows=[json.loads(x) for x in (p/'runs.jsonl').read_text().splitlines()]
g=collections.defaultdict(dict)
for r in rows:
 if not r['name'].startswith('overhead'):continue
 rep=r['name'].split('-')[1]
 for x in r['results']:g[(r['mode'],r['wal'],x['name'])].setdefault(rep,{})[r['diagnostics']]=x
for k,reps in g.items():
 rates=[100*(v['on']['ops_per_second']/v['off']['ops_per_second']-1) for v in reps.values()]
 p99=[100*(v['on']['p99_us']/v['off']['p99_us']-1) for v in reps.values()]
 print(k,'paired throughput',rates,'p99',p99)
