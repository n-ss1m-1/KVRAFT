# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
for r in range(6):
 for w in ['unique','update']:
  for f in ([1,2,4] if r%2==0 else [4,2,1]):run(f's2r{r}-4194304-{f}-{w}',4194304,f,w,'C')
for r in range(3):
 for f in ([1,2] if r%2==0 else [2,1]):run(f'walr{r}-4194304-{f}-update',4194304,f,'update','C',wal=1)
print('RESUMED C AND WAL COMPLETE',flush=True)
