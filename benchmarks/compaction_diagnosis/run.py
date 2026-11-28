# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,shutil,json,os,time
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-diagnosis-20261125';H=R/'benchmarks/results/compaction-acceptance-20261125';(O/'runs').mkdir(exist_ok=True);(O/'copies').mkdir(exist_ok=True)
def run(name,kind,mode,side=0,variant=0,source=None):
 d=O/'copies'/name;src=source or (H/'fixtures/multi-64' if side==0 else H/'copies/cost-r0-multi-64-c1');shutil.copytree(src,d)
 out=O/'runs'/name;args=[str(O/'build'/('bench_'+kind)),mode,str(d)]
 if mode=='hot':args += [str(variant),str(out)]
 elif mode=='write':args += [str(side),str(out)]
 env=os.environ.copy();env['DD_OUT']=str(out)+'.probe.json';a=time.time()
 with (O/'commands.jsonl').open('a') as f:f.write(json.dumps({'args':args,'DD_OUT':env['DD_OUT']})+'\n')
 try:
  p=subprocess.run(args,cwd=R,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=120)
  out.with_suffix('.stdout').write_text(p.stdout);out.with_suffix('.stderr').write_text(p.stderr);v=json.loads(p.stdout) if p.returncode==0 else {'failure':p.returncode}
 except subprocess.TimeoutExpired as e:v={'timeout':True};out.with_suffix('.stderr').write_text(str(e))
 v.update(name=name,kind=kind,side=side,variant=variant,process_s=time.time()-a);out.with_suffix('.json').write_text(json.dumps(v,indent=2));print(name,round(v['process_s'],3),flush=True)
 if 'failure' in v or 'timeout' in v:raise RuntimeError(name+' failed')
 return v
# Pilot preserves separate records and reuses exact historical representative fixture.
run('pilot-hot','plain','hot');run('pilot-hot-probe','probe','hot');run('pilot-write','probe','write',1)
# Formal reproduction / overhead: alternate source and diagnostic order, all builds completed.
for rnd in range(3):
 for mode in ['hot','write']:
  for side in ([0,1] if rnd%2==0 else [1,0]):
   for kind in (['plain','probe'] if rnd%2==0 else ['probe','plain']):run(f'r{rnd}-{mode}-s{side}-{kind}',kind,mode,side)
# Alternate fixed random hotspot sets, mechanism only.
for variant in [1,2]:
 for side in [0,1]:run(f'position-v{variant}-s{side}','probe','hot',side,variant)
run('compact-invariants','probe','compact')
print('DONE',flush=True)
