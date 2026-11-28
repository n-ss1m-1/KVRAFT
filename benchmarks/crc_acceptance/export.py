# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,csv,collections,statistics as st
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/crc-table-acceptance-20261118';rows=[json.loads(x) for x in (o/'results.jsonl').read_text().splitlines()]
for tool,name in [('crc-micro','micro-rounds.csv'),('acceptance','query-rounds.csv')]:
 vs=[{k:v for k,v in x.items() if k!='command'} for x in rows if x['tool']==tool and (tool=='crc-micro' or 'case' in x)]
 with (o/name).open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=list(vs[0]));w.writeheader();w.writerows(vs)
lines=['Query throughput ratios computed from current-run medians:']
for nt in [1,64]:
 for case in ['latest','oldest','missing']:
  groups={v:[x['ops_per_second'] for x in rows if x.get('case')==case and x['tables']==nt and x['version']==v] for v in ['old','new']}
  lines.append(f'{nt} tables {case}: {st.median(groups["new"])/st.median(groups["old"]):.3f}x; throughput range old {min(groups["old"]):.0f}..{max(groups["old"]):.0f}, new {min(groups["new"]):.0f}..{max(groups["new"]):.0f}')
(o/'ratios.txt').write_text('\n'.join(lines)+'\n')
