from pathlib import Path
import json,collections,statistics
r=Path('/home/truth/Code/my_kvraft');out=r/'benchmarks/results/diagnostics-20261116-first'
rows=[json.loads(s) for s in (out/'runs.jsonl').read_text().splitlines() if s]
g=collections.defaultdict(dict)
for row in rows:
 if not row['name'].startswith('overhead'):continue
 for result in row['results']:
  k=(row['mode'],row['wal'],result['name']);g[k].setdefault(row['diagnostics'],[]).append(result)
for k,rs in g.items():
 if not all(x in rs for x in ['on','off']):continue
 a=statistics.median(x['ops_per_second'] for x in rs['off']);b=statistics.median(x['ops_per_second'] for x in rs['on'])
 print(k,len(rs['off']),len(rs['on']),'throughput %',round(100*(b/a-1),2))
