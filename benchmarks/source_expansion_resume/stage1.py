# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
configs=[(b,f) for b in [262144,1048576,4194304] for f in [1,2,4]]
for r in range(3):
 cs=configs[r*3:]+configs[:r*3]
 if r%2:cs.reverse()
 for w in ['unique','update']:
  for b,f in cs:run(f's1r{r}-{b}-{f}-{w}',b,f,w)
print('STAGE1 COMPLETE',flush=True)