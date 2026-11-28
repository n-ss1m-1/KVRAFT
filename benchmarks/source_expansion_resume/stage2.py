# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
from statistics import median
# Execute after stage1. Selection uses physical work, paired latency and task budget, not throughput rank.
vs=[json.loads(p.read_text()) for p in (O/'runs').glob('s1*.json')]
assert len(vs)==54 and all(v['verified'] for v in vs)
selection={'small_key_configs':[[1048576,1],[1048576,4],[4194304,1],[4194304,4]],'large_configs':[[4194304,1],[4194304,2],[4194304,4]],'reason':'Retain no-expansion anchors and equal-ceiling pair at small scale; C fixes default base budget and varies factor to separate expansion on large files. Include factor4 to examine larger tasks, not because highest request throughput.'}
(O/'stage2-selection.json').write_text(json.dumps(selection,indent=2))
for r in range(3,6):
 cs=selection['small_key_configs'];cs=cs if r%2==0 else list(reversed(cs))
 for w in ['unique','update']:
  for b,f in cs:run(f's1r{r}-{b}-{f}-{w}',b,f,w)
# pilot large scale, formal list is decided from 54 completed rounds, pilot retained separately
for f in [1,2,4]:
 for w in ['unique','update']:run(f'cpilot-4194304-{f}-{w}',4194304,f,w,'C',kind='mechanism')
for r in range(6):
 cs=[1,2,4] if r%2==0 else [4,2,1]
 for w in ['unique','update']:
  for f in cs:run(f's2r{r}-4194304-{f}-{w}',4194304,f,w,'C')
for r in range(3):
 for f in ([1,2] if r%2==0 else [2,1]):run(f'walr{r}-4194304-{f}-update',4194304,f,'update','C',wal=1)
print('STAGE2 COMPLETE',flush=True)