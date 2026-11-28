# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
# Full mechanism coverage once for small matrix, representative pairs repeated to 3 samples including pilots.
for b,f in [(262144,1),(262144,2),(1048576,2),(4194304,2),(4194304,4)]:
 for w in ['unique','update']:run(f'matrixm-{b}-{f}-{w}',b,f,w,kind='mechanism')
for r in range(2):
 cs=[(1048576,1),(1048576,4),(4194304,1),(4194304,4)]
 if r%2:cs.reverse()
 for w in ['unique','update']:
  for b,f in cs:run(f'm{r}-{b}-{f}-{w}',b,f,w,kind='mechanism')
 for w in ['unique','update']:
  for f in ([1,2,4] if r%2==0 else [4,2,1]):run(f'cm{r}-4194304-{f}-{w}',4194304,f,w,'C',kind='mechanism')
# Detailed overhead pairs, not included in main mechanism aggregate.
for r in range(3):
 for kind in (['observer','mechanism'] if r%2==0 else ['mechanism','observer']):run(f'overhead{r}-{kind}',1048576,4,'unique',kind=kind)
# Limited query: equal-ceiling pair at A, factor1/2/4 at C, live/missing, no cache and sufficient cache.
for r in range(3):
 for scale,cs in [('A',[(1048576,4),(4194304,1)]),('C',[(4194304,1),(4194304,2),(4194304,4)])]:
  if r%2:cs.reverse()
  for w in ['unique','update']:
   for b,f in cs:
    prefix='s1' if scale=='A' else 's2';fixture=json.loads((O/'runs'/f'{prefix}r{r}-{b}-{f}-{w}.json').read_text());cap=sum(t['bytes'] for t in fixture['final'])
    for query in ['live','missing']:
     for cache in [0,cap]:
      name=f'q{r}-{scale}-{b}-{f}-{w}-{query}-c{int(bool(cache))}'
      v=run(name,b,f,query,scale,n=4096,mode='query',db=Path(fixture['db']),cap=cache);v['fixture_work']=w;(O/'runs'/(name+'.json')).write_text(json.dumps(v,indent=2))
print('MECHANISM AND QUERIES COMPLETE',flush=True)