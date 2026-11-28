# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,random,time,sys
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/batch-acceptance-20261120'
def run(name,wal,batch,n=16384,timed=1,mode='formal',work='unique',mech=False):
 prefix=o/name
 if prefix.with_suffix('.json').exists():raise RuntimeError('existing result '+name)
 args=[str(o/'build'/('batch-mechanism' if mech else 'batch-bench')),str(o/'databases'/name),str(wal),str(batch),str(n),str(timed),mode,work,str(prefix)]
 start=time.monotonic()
 try:
  p=subprocess.run(args,text=True,capture_output=True,timeout=300)
  prefix.with_suffix('.stdout').write_text(p.stdout);prefix.with_suffix('.stderr').write_text(p.stderr)
  data=json.loads(p.stdout);data.update(returncode=p.returncode)
 except subprocess.TimeoutExpired as e:
  data=dict(status='timeout');prefix.with_suffix('.stderr').write_text(str(e))
 data.update(name=name,wal=wal,batch=batch,round=rep if 'rep' in globals() else -1,kind=sys.argv[1],mode=mode,work=work,command=args,process_seconds=time.monotonic()-start)
 prefix.with_suffix('.json').write_text(json.dumps(data,indent=2))
 with (o/'runs.jsonl').open('a') as f:f.write(json.dumps(data)+'\n')
 print(name,data['status'],round(data.get('submit_ns',0)/1e9,3),'s',flush=True)
 if data['status']!='ok' or data.get('returncode',0):raise RuntimeError(data)
 if mech:
  c=data['mechanism'][1];expected=(n+max(batch,1)-1)//max(batch,1) if wal else 0
  assert c['data_sync']==expected and c['records']==expected,(data,expected)
  assert c['bytes']==(161*n+12*expected if wal else 0),(c,n,expected)
 return data
kind=sys.argv[1]
if kind=='pilot':
 for wal,batch in [(1,0),(1,64),(0,0),(0,1024)]:
  run(f'pilot-v2-{wal}-{batch}',wal,batch,n=2048,mech=True)
elif kind=='formal':
 for rep in range(3):
  cases=[(w,b) for w in [0,1] for b in [0,1,4,16,64,256,1024]]
  random.Random(20261115+rep).shuffle(cases)
  for w,b in cases:run(f'formal-r{rep}-{w}-{b}',w,b)
elif kind=='extras':
 for w in [0,1]:
  for b in [0,1,4,16,64,256,1024]:run(f'mechanism-{w}-{b}',w,b,mode='check',mech=True)
 for b in [0,1,4,16,64,256,1024]:run(f'recovery-{b}',1,b,n=257,mode='recovery')
 for rep in range(3):
  cases=[(b,t) for b in [0,1,16,1024] for t in [0,1]]
  random.Random(20261215+rep).shuffle(cases)
  for b,t in cases:run(f'overhead-r{rep}-{b}-{t}',0,b,timed=t)
elif kind=='mixed':
 for rep in range(3):
  cases=[(w,b) for w in [0,1] for b in [0,16,256]]
  random.Random(20261315+rep).shuffle(cases)
  for w,b in cases:run(f'mixed-r{rep}-{w}-{b}',w,b,work='mixed')

elif kind=='paired':
 for rep in [3,4]:
  for w,b in ([(1,1),(1,0)] if rep==3 else [(1,0),(1,1)]):
   run(f'paired-r{rep}-{w}-{b}',w,b)

elif kind=='totalonly':
 for rep in range(3):
  cases=[0,1,4,16,64,256,1024]
  random.Random(20261415+rep).shuffle(cases)
  for b in cases:run(f'totalonly-r{rep}-0-{b}',0,b,timed=0)
