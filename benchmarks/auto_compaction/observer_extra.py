# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
# Observer showed >30% timing spread; append paired rounds, no rebuilding.
for r in range(3,6):
 for work in ['unique','update']:
  for a in ([0,1] if r%2==0 else [1,0]):run(f'o{r}-{work}-a{a}','observer','settle',a,0,work)
print('EXTRA DONE',flush=True)