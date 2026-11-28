# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

exec(open('/home/truth/Code/my_kvraft/benchmarks/compaction_acceptance/run.py').read().split('# Pilot before')[0])
cap=json.loads((O/'capacities.json').read_text())['capacity']
post=O/'copies/cost-r0-multi-64-c1'
for rnd in [3,4]:
 for target in ([1048576,4194304] if rnd==3 else [4194304,1048576]):
  name=f'extra-split-r{rnd}-target{target}';d=copy(O/'fixtures/multi-64',name)
  call(name,['compact',d,1,target],{'round':rnd,'fixture':'multi-64','kind':'split' if target==1048576 else 'cost'})
 for cache in [0,cap]:
  for side in (['after','before'] if rnd==3 else ['before','after']):
   name=f'extra-query-r{rnd}-multi-64-cache{cache}-{side}';d=copy(O/'fixtures/multi-64' if side=='before' else post,name)
   call(name,['query',d,cache,8192,O/'runs'/name],{'round':rnd,'fixture':'multi-64','kind':'query','side':side})
