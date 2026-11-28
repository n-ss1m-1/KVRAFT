# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,time,shutil,struct,statistics
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-acceptance-20261125';B=O/'perf-build/compaction-bench'
(O/'runs').mkdir(exist_ok=True);(O/'fixtures').mkdir(exist_ok=True);(O/'copies').mkdir(exist_ok=True)
def call(name,args,meta=None,timeout=180):
 base=O/'runs'/name
 if base.with_suffix('.json').exists():raise RuntimeError('refuse overwrite '+name)
 cmd=[str(B)]+list(map(str,args));a=time.time()
 with (O/'commands.jsonl').open('a') as f:f.write(json.dumps(cmd)+'\n')
 try:
  p=subprocess.run(cmd,cwd=R,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout)
  base.with_suffix('.stdout').write_text(p.stdout);base.with_suffix('.stderr').write_text(p.stderr)
  v=json.loads(p.stdout) if p.returncode==0 else {'failure':p.returncode}
 except subprocess.TimeoutExpired as e:
  base.with_suffix('.stderr').write_text(str(e));v={'timeout':True}
 v.update(meta or {});v['elapsed_process_s']=time.time()-a;v['name']=name
 base.with_suffix('.json').write_text(json.dumps(v,indent=2))
 print(name,round(v['elapsed_process_s'],3),flush=True)
 if 'failure' in v or 'timeout' in v:raise RuntimeError(name+' failed; sampling stopped')
 return v
def copy(src,name):
 d=O/'copies'/name;shutil.copytree(src,d);return d
# Pilot before choosing formal counts.
pilot=O/'fixtures/pilot';call('pilot-create',['create',pilot,4096,16,1]);pc=copy(pilot,'pilot-compact');pv=call('pilot-compact',['compact',pc,1,4194304]);call('pilot-query',['query',pc,0,1024,O/'runs/pilot-query']);pi=copy(pilot,'pilot-impact');call('pilot-impact',['impact',pi,1,1,32,O/'runs/pilot-impact',pv['api_ns']])
# Immutable closed fixtures, no WAL, one Flush per requested input table.
fixtures={}
for multi in [0,1]:
 for tables in [16,64]:
  label=('multi' if multi else 'unique')+'-'+str(tables);d=O/'fixtures'/label
  call('fixture-'+label,['create',d,65536,tables,multi]);fixtures[label]=d
# Cost baseline vs compaction, alternating paired order; split target representative.
post={};windows=[]
for rnd in range(3):
 for label,src in fixtures.items():
  for run in ([0,1] if rnd%2==0 else [1,0]):
   name=f'cost-r{rnd}-{label}-c{run}';d=copy(src,name)
   v=call(name,['compact',d,run,4194304],{'round':rnd,'fixture':label,'kind':'cost'})
   if run and rnd==0:post[label]=d
   if run and label=='multi-64':windows.append(v['api_ns'])
 for target in [1048576]:
  name=f'split-r{rnd}-multi-64';d=copy(fixtures['multi-64'],name)
  call(name,['compact',d,1,target],{'round':rnd,'fixture':'multi-64','kind':'split'})
def charge(d):
 total=blocks=0
 for f in d.glob('*.sst'):
  b=f.read_bytes();off,size=struct.unpack_from('<QQ',b[-48:-4],28);idx=b[off:off+size];n=struct.unpack_from('<I',idx)[0];pos=4
  for _ in range(n):
   ln=struct.unpack_from('<I',idx,pos)[0];pos+=4+ln+8+1+8
   sz=struct.unpack_from('<I',idx,pos)[0];pos+=4;total+=sz;blocks+=1
 return {'charge_bytes':total,'blocks':blocks}
charges={label:{'before':charge(src),'after':charge(post[label])} for label,src in fixtures.items()}
cap=max(s['charge_bytes'] for c in charges.values() for s in c.values())+4096
(O/'capacities.json').write_text(json.dumps({'capacity':cap,'datasets':charges},indent=2))
for rnd in range(3):
 for label,src in fixtures.items():
  for cache in [0,cap]:
   for side in (['before','after'] if rnd%2==0 else ['after','before']):
    name=f'query-r{rnd}-{label}-cache{cache}-{side}';d=copy(src if side=='before' else post[label],name)
    call(name,['query',d,cache,8192,O/'runs'/name],{'round':rnd,'fixture':label,'kind':'query','side':side})
# Full time series with matching fixed work count; control marks median API-duration window.
window=statistics.median(windows)
for rnd in range(3):
 for writing in [0,1]:
  for run in ([0,1] if rnd%2==0 else [1,0]):
   name=f'impact-r{rnd}-write{writing}-c{run}';d=copy(fixtures['multi-64'],name)
   call(name,['impact',d,run,writing,256 if writing else 131072,O/'runs'/name,window],{'round':rnd,'fixture':'multi-64','kind':'impact'})
print('ALL COMPLETE',flush=True)
