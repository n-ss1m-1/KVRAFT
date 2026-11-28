# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
# Additional repetitions only for key WAL-off timing comparisons, preserve all first rounds.
for r in range(3,6):
 for work in ['unique','update']:
  cs=[('off','A',0,131072),('A','A',1,131072),('B','B',1,131072),('Coff','C',0,524288),('C','C',1,524288)]
  if r%2:cs.reverse()
  for label,cfg,a,n in cs:run(f'f{r}-{work}-{label}',cfg,a,work,n)
# Matched core/observer no-convergence write path checks: same workload and snapshot.
for r in range(3):
 for kind in (['previous','observer'] if r%2==0 else ['observer','previous']):run(f'overhead{r}-{kind}','A',1,'unique',131072,kind,mode='write')
# Static query subset: original live trace, independent empty cache and same OS prewarm.
for r in range(3):
 for work in ['unique','update']:
  for label,cfg in ([('off','A'),('A','A'),('B','B')] if r%2==0 else [('B','B'),('A','A'),('off','A')]):
   v=json.loads((O/'runs'/f'f{r}-{work}-{label}.json').read_text());cap=sum(t['bytes'] for t in v['final'])
   for cache in [0,cap]:run(f'q{r}-{work}-{label}-c{int(bool(cache))}',cfg,1,'live',4096,mode='query',db=Path(v['db']),cap=cache)
print('EXTRAS COMPLETE',flush=True)