# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

exec(open('/home/truth/Code/my_kvraft/benchmarks/dual_background/run.py').read().split('for version in [')[0])
for rnd in range(5,8):
 order=['engine-snapshot','serial'] if rnd%2 else ['serial','engine-snapshot']
 for comp in [0,1]:
  for version in order:run(f'r{rnd}-write-on-c{comp}-{version}',version,comp=comp,wal=1)
 run(f'r{rnd}-control-current-serial','current-serial')
