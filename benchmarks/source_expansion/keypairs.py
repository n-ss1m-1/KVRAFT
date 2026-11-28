# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
# Close the factor1/2/4 paired evidence gap at small scale without expanding other boundary groups.
for r in range(3,6):
 for w in ['unique','update']:
  for b in ([1048576,4194304] if r%2==0 else [4194304,1048576]):run(f's1r{r}-{b}-2-{w}',b,2,w)