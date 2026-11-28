# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,random,time,sys
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/group-commit-acceptance-20261121-v2'
def run(name,w,t,g,n=16384,b=0,budget=1048576,timed=1,readers=0,mode='formal',mech=False):
 if (o/(name+'.json')).exists():raise RuntimeError('refuse existing '+name)
 args=[str(o/'build'/('group-mechanism' if mech else 'group-bench')),str(o/'databases'/name),str(w),str(t),str(g),str(budget),str(b),str(n),str(timed),str(readers),mode,str(o/name)]
 start=time.monotonic()
 try:
  p=subprocess.run(args,text=True,capture_output=True,timeout=180)
  (o/(name+'.stdout')).write_text(p.stdout);(o/(name+'.stderr')).write_text(p.stderr)
  data=json.loads(p.stdout);data['returncode']=p.returncode
 except subprocess.TimeoutExpired as e:data=dict(status='timeout',error=str(e))
 except Exception as e:data=dict(status='harness-failed',error=str(e))
 data.update(name=name,kind=sys.argv[1],wal=w,threads=t,limit=g,budget=budget,batch=b,n=n,timed=timed,readers=readers,mode=mode,mechanism_enabled=mech,command=args,process_seconds=time.monotonic()-start)
 (o/(name+'.json')).write_text(json.dumps(data,indent=2))
 with (o/'runs.jsonl').open('a') as f:f.write(json.dumps(data)+'\n')
 print(name,data['status'],round(data.get('request_ns',0)/1e9,4),'s',flush=True)
 if data['status']!='ok' or data.get('returncode',0):raise RuntimeError('STOP: engine/data failure '+name)
 assert data['operations']==n
 if mech and mode!='recovery':
  c=data['mechanism'][1];assert c['partial']==0
  expected=t*((n//t+max(b,1)-1)//max(b,1))
  assert c['records']==(expected if w else 0)
  assert c['wal_bytes']==(161*n+12*expected if w else 0)
  if w:
   assert c['data_sync'] and c['records']/c['data_sync']<=min(t,g)
   if g==1:assert c['data_sync']==expected
 return data
kind=sys.argv[1]
if kind=='pilot':
 for w,t,g in [(1,1,1),(1,8,1),(1,8,32),(0,8,32)]:run(f'pilot-{w}-{t}-{g}',w,t,g,n=1024,mech=True)
elif kind=='formal':
 for rep in range(3):
  pairs=[(w,t) for w in [0,1] for t in [1,2,4,8]]
  random.Random(20261115+rep).shuffle(pairs)
  for i,(w,t) in enumerate(pairs):
   for g in ([1,32] if (rep+i)%2==0 else [32,1]):run(f'formal-r{rep}-{w}-t{t}-g{g}',w,t,g)
elif kind=='mechanism':
 for w in [0,1]:
  for t in [1,2,4,8]:
   for g in [1,32]:run(f'mechanism-{w}-t{t}-g{g}',w,t,g,n=4096,mech=True)
 for t in [1,8]:
  for g in [1,32]:run(f'recovery-t{t}-g{g}',1,t,g,n=1024,mode='recovery')
elif kind=='extensions':
 for rep in range(3):
  cases=[(g,0,1048576,0) for g in [2,4,8]]
  cases += [(g,b,1048576,0) for g in [1,32] for b in [16,64]]
  cases += [(32,1024,budget,0) for budget in [200000,1048576]]
  cases += [(g,0,1048576,1) for g in [1,32]]
  random.Random(20261215+rep).shuffle(cases)
  for g,b,budget,rd in cases:run(f'extension-r{rep}-g{g}-b{b}-bytes{budget}-rd{rd}',1,8,g,b=b,budget=budget,readers=rd)
elif kind=='extension-mechanism':
 for g in [2,4,8]:run(f'ext-mechanism-g{g}',1,8,g,n=4096,mech=True)
 for g in [1,32]:
  for b in [16,64]:run(f'ext-mechanism-g{g}-b{b}',1,8,g,b=b,mech=True)
 for budget in [200000,1048576]:run(f'ext-mechanism-byte{budget}',1,8,32,b=1024,budget=budget,mech=True)
elif kind=='overhead':
 for rep in range(3):
  pairs=[(t,g) for t in [1,8] for g in [1,32]]
  random.Random(20261315+rep).shuffle(pairs)
  for t,g in pairs:
   for timed in ([0,1] if rep%2 else [1,0]):run(f'overhead-r{rep}-t{t}-g{g}-timed{timed}',0,t,g,timed=timed)

elif kind=='paired':
 for rep in range(2):
  for t in [1,8]:
   for g in ([1,32] if rep==0 else [32,1]):run(f'paired-r{rep}-t{t}-g{g}',1,t,g)
