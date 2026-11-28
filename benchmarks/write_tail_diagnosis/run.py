# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,shutil,time,os,csv
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/write-tail-diagnosis-20261125';F=R/'benchmarks/results/compaction-acceptance-20261125/fixtures/multi-64'
(O/'runs').mkdir(exist_ok=True);(O/'copies').mkdir(exist_ok=True)
used=0;seconds=0

def run(name,kind,comp):
 global used,seconds
 if used>=36 or seconds>=1800:return False
 d=O/'copies'/name;shutil.copytree(F,d);out=O/'runs'/name
 cmd=[str(O/'build'/('bench_'+kind)),'write',str(d),str(comp),'0','1024',str(out)]
 env=os.environ.copy();env['DD_OUT']=str(out)+'.probe.json'
 with (O/'commands.jsonl').open('a') as f:f.write(json.dumps(dict(cmd=cmd,DD_OUT=env['DD_OUT']))+'\n')
 start=time.monotonic();used+=1
 try:
  p=subprocess.run(cmd,cwd=R,env=env,capture_output=True,text=True,timeout=min(180,1800-seconds))
  out.with_suffix('.stdout').write_text(p.stdout);out.with_suffix('.stderr').write_text(p.stderr)
  v=json.loads(p.stdout) if p.returncode==0 else dict(failed=p.returncode)
 except subprocess.TimeoutExpired as e:v=dict(timeout=True);out.with_suffix('.stderr').write_text(str(e))
 elapsed=time.monotonic()-start;seconds+=elapsed
 v.update(name=name,kind=kind,comp=comp,process_s=elapsed,sampling_s=seconds,run_number=used)
 out.with_suffix('.json').write_text(json.dumps(v,indent=2))
 print(name,round(elapsed,2),'max_ms',round(v.get('requests',{}).get('max_ns',0)/1e6,3),flush=True)
 if 'failed' in v or 'timeout' in v:raise RuntimeError('failure stops sampling '+name)
 if kind=='probe':
  q=json.loads(Path(env['DD_OUT']).read_text());assert q['events_dropped']==0 and q['overflow']==0
  e=q['events'];assert sum(x['metric']=='ImmutableRemoved' for x in e)==17
  assert sum(x['metric']=='Claim' for x in e)==17+comp
  assert len(set(x['slot'] for x in e if x['phase']==5))==1
  if comp:assert set(x['slot'] for x in e if x['phase']==5).isdisjoint(x['slot'] for x in e if x['phase']==6)
 return True

for c in [0,1]:run(f'pilot-c{c}','probe',c)
# Interleave off/on and scenario; no other workload or build runs concurrently.
for r in range(6):
 for c in ([0,1] if r%2==0 else [1,0]):
  for k in (['engine-snapshot','probe'] if r%2==0 else ['probe','engine-snapshot']):run(f'r{r}-c{c}-{k}',k,c)
# Extend only diagnosed scenario(s) with no abnormal API batch captured. Maximum36 including pilots.
for r in range(6,11):
 for c in ([0,1] if r%2==0 else [1,0]):
  vals=[json.loads(p.read_text()) for p in (O/'runs').glob('*.json') if not p.name.endswith('.probe.json')]
  caught=any(v['kind']=='probe' and v['comp']==c and v['requests']['max_ns']>100000000 for v in vals)
  if not caught:run(f'r{r}-c{c}-probe','probe',c)
(O/'budget.json').write_text(json.dumps(dict(runs=used,process_wall_s=seconds,limit_runs=36,limit_s=1800),indent=2))
print('FINISHED',used,seconds,flush=True)