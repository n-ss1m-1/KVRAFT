# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,shutil,time,os
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/dual-background-20261125';F=R/'benchmarks/results/compaction-acceptance-20261125/fixtures/multi-64';(O/'runs').mkdir(exist_ok=True);(O/'copies').mkdir(exist_ok=True)
def run(name,version,mode='write',comp=1,wal=0,count=1024):
 d=O/'copies'/name;shutil.copytree(F,d);out=O/'runs'/name;cmd=[str(O/'build'/('bench_'+version)),mode,str(d)]
 if mode!='compact':cmd+=list(map(str,[comp,wal,count,out]))
 env=os.environ.copy();env['DD_OUT']=str(out)+'.probe.json'
 with (O/'commands.jsonl').open('a') as f:f.write(json.dumps({'cmd':cmd,'DD_OUT':env['DD_OUT']})+'\n')
 start=time.time()
 try:
  p=subprocess.run(cmd,cwd=R,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180)
  out.with_suffix('.stdout').write_text(p.stdout);out.with_suffix('.stderr').write_text(p.stderr);v=json.loads(p.stdout) if p.returncode==0 else {'failed':p.returncode}
 except subprocess.TimeoutExpired as e:v={'timeout':True};out.with_suffix('.stderr').write_text(str(e))
 v.update(name=name,version=version,scenario=mode,comp=comp,wal=wal,process_s=time.time()-start);out.with_suffix('.json').write_text(json.dumps(v,indent=2));print(name,round(v['process_s'],2),flush=True)
 if 'failed' in v or 'timeout' in v:raise RuntimeError('failure stop '+name)
 return v
for version in ['serial','engine-snapshot','current-serial']:run('pilot-'+version,version)
for rnd in range(8):
 order=['serial','engine-snapshot'] if rnd%2==0 else ['engine-snapshot','serial']
 for comp in [0,1]:
  for version in order:run(f'r{rnd}-write-off-c{comp}-{version}',version,comp=comp)
 if rnd<5:
  for comp in [0,1]:
   for version in order:run(f'r{rnd}-write-on-c{comp}-{version}',version,comp=comp,wal=1)
  for version in order:run(f'r{rnd}-compact-{version}',version,'compact')
  for comp in [0,1]:
   for version in order:run(f'r{rnd}-read-c{comp}-{version}',version,'read',comp,0,65536)
 if rnd<5:run(f'r{rnd}-control-current-serial','current-serial')
# Separate mechanism, full workload, independent thread slots, no prior fine Add timings.
for rnd in range(3):
 for version in (['serial-probe','engine-snapshot-probe'] if rnd%2==0 else ['engine-snapshot-probe','serial-probe']):run(f'm{rnd}-{version}',version)
print('DONE',flush=True)
