# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

exec(open('/home/truth/Code/my_kvraft/benchmarks/compaction_diagnosis/run.py').read().split('# Pilot preserves')[0])
# Targeted extra uninstrumented paired rounds: no forced timing.
for rnd in range(3,6):
 for side in ([1,0] if rnd%2 else [0,1]):run(f'r{rnd}-write-s{side}-plain','plain','write',side)
