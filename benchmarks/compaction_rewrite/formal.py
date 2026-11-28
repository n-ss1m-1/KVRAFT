# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
# Same small scale for OFF/A/B; C uses a separate larger scale and OFF baseline.
for r in range(3):
 for work in ['unique','update']:
  configs=[('off','A',0,131072),('A','A',1,131072),('B','B',1,131072),('Coff','C',0,524288),('C','C',1,524288)]
  if r%2:configs.reverse()
  else:configs=configs[r:]+configs[:r]
  for label,cfg,a,n in configs:run(f'f{r}-{work}-{label}',cfg,a,work,n)
# representative WAL-on A/B update, not a second full matrix
for r in range(3):
 for cfg in (['A','B'] if r%2==0 else ['B','A']):run(f'w{r}-update-{cfg}',cfg,1,'update',131072,wal=1)
# paired detailed mechanism rounds, plus prior three pilot mechanisms
for r in range(2):
 for work in ['unique','update']:
  for cfg,n in ([('A',131072),('B',131072),('C',524288)] if r==0 else [('C',524288),('B',131072),('A',131072)]):run(f'm{r}-{work}-{cfg}',cfg,1,work,n,'mechanism')
print('FORMAL COMPLETE',flush=True)