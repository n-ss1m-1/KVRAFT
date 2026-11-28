# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
# Bounded extra mechanism overhead checks, separate from all reported formal medians.
for r in range(3):
 for k in (['observer','mechanism'] if r%2==0 else ['mechanism','observer']):run(f'diag{r}-{k}','A',1,'unique',131072,k)
# Candidate counts reuse prior verified identical engine snapshot instrumentation, read-only fixtures.
# Its options exactly match A/B layouts (3 levels); C is excluded from this particular probe.
for work in ['unique','update']:
 for label in ['off','A','B']:
  v=json.loads((O/'runs'/f'f0-{work}-{label}.json').read_text());out=O/'runs'/f'candidate-{work}-{label}'
  args=[str(R/'benchmarks/results/auto-compaction-20261127/build/bench_probe'),'query',v['db'],str(out),'0','live','4096']
  with (O/'commands.jsonl').open('a') as f:f.write(json.dumps({'argv':args})+'\n')
  p=subprocess.run(args,capture_output=True,text=True,timeout=180);out.with_suffix('.stdout').write_text(p.stdout);out.with_suffix('.stderr').write_text(p.stderr);assert p.returncode==0
  j=json.loads(p.stdout);assert j['verified'];out.with_suffix('.json').write_text(json.dumps(j,indent=2));print(out.name,j['candidate_tables'],flush=True)
for r in range(3):
 for work in ['unique','update']:
  for label in (['Coff','C'] if r%2==0 else ['C','Coff']):
   v=json.loads((O/'runs'/f'f{r}-{work}-{label}.json').read_text());cap=sum(t['bytes'] for t in v['final'])
   for cache in [0,cap]:run(f'q{r}-{work}-{label}-c{int(bool(cache))}','C',1,'live',4096,mode='query',db=Path(v['db']),cap=cache)
print('MECHANISMS COMPLETE',flush=True)