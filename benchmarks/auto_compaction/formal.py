# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
import statistics
# Core online matrix; single current engine, auto switch only.
for r in range(3):
 for wal in [0,1]:
  for work in ['unique','update']:
   for a in ([0,1] if r%2==0 else [1,0]):run(f'r{r}-{work}-w{wal}-a{a}','engine','write',a,wal,work)
# Significant wall-time variation: preserve all, extend only affected pairs to six.
for wal in [0,1]:
 for work in ['unique','update']:
  noisy=False
  for a in [0,1]:
   vs=[json.loads((O/'runs'/f'r{r}-{work}-w{wal}-a{a}.json').read_text())['writes']['wall_ns'] for r in range(3)]
   noisy |= max(vs)>min(vs)*1.3
  if noisy:
   for r in range(3,6):
    for a in ([0,1] if r%2==0 else [1,0]):run(f'r{r}-{work}-w{wal}-a{a}','engine','write',a,wal,work)
# Separate mechanism/convergence processes. Input-record counting adds work, not formal throughput.
for r in range(3):
 for work in ['unique','update']:
  for a in ([0,1] if r%2==0 else [1,0]):run(f'm{r}-{work}-a{a}','probe','settle',a,0,work)
# Static query: independently converged/frozen layouts; normal engine diagnostics off, auto off.
for r in range(3):
 for work in ['unique','update']:
  for load in (['live','missing','hot'] if work=='unique' else ['live','missing','deleted','hot']):
   for cache in [0,1]:
    for a in ([0,1] if r%2==0 else [1,0]):
     origin=json.loads((O/'runs'/f'm{r}-{work}-a{a}.json').read_text());cap=sum(t['bytes'] for t in origin['final']) if cache else 0
     run(f'q{r}-{work}-{load}-c{cache}-a{a}','engine','query',a,0,load,4096,O/'db'/f'm{r}-{work}-a{a}',cap)
for r in range(3):
 for a in ([0,1] if r%2==0 else [1,0]):run(f'x{r}-mixed-a{a}','engine','write',a,0,'mixed')
# Small candidate-count mechanism, separate from query performance.
for a in [0,1]:
 for cache in [0,1]:
  origin=json.loads((O/'runs'/f'm0-update-a{a}.json').read_text());cap=sum(t['bytes'] for t in origin['final']) if cache else 0
  run(f'query-mechanism-c{cache}-a{a}','probe','query',a,0,'live',4096,O/'db'/f'm0-update-a{a}',cap)
print('DONE',flush=True)