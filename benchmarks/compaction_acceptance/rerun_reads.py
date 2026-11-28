# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

exec(open('/home/truth/Code/my_kvraft/benchmarks/compaction_acceptance/run.py').read().split('# Pilot before')[0])
window=statistics.median(json.loads(p.read_text())['api_ns'] for p in (O/'runs').glob('cost-r*-multi-64-c1.json'))
for rnd in range(3):
 for run in ([0,1] if rnd%2==0 else [1,0]):
  name=f'impact-v2-r{rnd}-write0-c{run}';d=copy(O/'fixtures/multi-64',name)
  call(name,['impact',d,run,0,131072,O/'runs'/name,window],{'round':rnd,'fixture':'multi-64','kind':'impact-v2'})
