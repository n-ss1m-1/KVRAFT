import argparse,json,subprocess,time,random,hashlib,collections
from pathlib import Path
r=Path('/home/truth/Code/my_kvraft');out=r/'benchmarks/results/diagnostics-20261116-first'
def run(name,on,mode,n,wal,limit,tables):
 prefix=out/name; args=[str(r/f'build/diag-{on}/diagnose'),str(out/'databases'/name),mode,str(n),str(wal),str(limit),str(tables),'20261115',str(prefix)]
 start=time.monotonic()
 try:p=subprocess.run(args,text=True,capture_output=True,timeout=240)
 except subprocess.TimeoutExpired as e:
  (out/f'{name}.timeout.json').write_text(json.dumps(dict(timeout_seconds=240,command=args)));raise
 (out/f'{name}.stderr.txt').write_text(p.stderr);(out/f'{name}.stdout.json').write_text(p.stdout)
 if p.returncode:raise RuntimeError((name,p.returncode,p.stderr))
 row=json.loads(p.stdout)|dict(name=name,diagnostics=on,mode=mode,wal=wal,limit=limit,configured_tables=tables,process_seconds=time.monotonic()-start,command=args)
 if on=='on':
  stats=json.loads(Path(str(prefix)+'.stats.json').read_text());assert stats['dropped_threads']==0
  cells=stats['cells']
  assert all(c['failed']==0 for c in cells)
  def total(phase,op,metric,field='count'):return sum(c[field] for c in cells if c['phase']==phase and c['op']==op and c['metric']==metric)
  if mode=='write':
   assert total(1,1,'memtable_add')==n
   assert total(1,1,'wal_sync')==(n if wal else 0)
   assert total(1,1,'first_lock')==n
   row['automatic_rotations']=total(1,1,'rotate')
   assert total(1,5,'sst_build')==row['automatic_rotations']
   assert total(2,5,'sst_build')==total(2,4,'rotate')
  else:
   assert row['tables']==tables
   for phase in range(6,12):
    assert total(phase,3,'first_lock')==n
    assert total(phase,3,'tables_checked')==n*(tables if phase in (8,9,10,11) else 1)
    assert total(phase,3,'bloom_checks')==total(phase,3,'tables_checked')
    assert total(phase,3,'blocks')==total(phase,3,'block_construct_validate')
    assert total(phase,3,'blocks')==total(phase,3,'block_get')
    assert total(phase,3,'pread_all')==2*total(phase,3,'blocks')
 with (out/'runs.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
 print(name,'done',round(row['process_seconds'],3),flush=True)
 return row
p=argparse.ArgumentParser();p.add_argument('stage');a=p.parse_args()
(out/'databases').mkdir(exist_ok=True)
if a.stage=='pilot':
 for n in (8192,16384):
  v=run(f'pilot-write-{n}','on','write',n,1,1048576,0)
  if v['automatic_rotations']>=3:
   (out/'selected-n.json').write_text(json.dumps({'n':n,'automatic_rotations':v['automatic_rotations']}));break
elif a.stage=='overhead':
 cases=[('write',8192,0,65536,0),('write',int(json.loads((out/'selected-n.json').read_text())['n']),1,1048576,0),('query',8192,0,9223372036854775807,16)]
 for rep in range(3):
  order=cases.copy();random.Random(20261115+rep).shuffle(order)
  for i,(mode,n,wal,limit,nt) in enumerate(order):
   switches=['off','on'];random.Random(20261115+rep*10+i).shuffle(switches)
   for on in switches:run(f'overhead-r{rep}-{mode}-w{wal}-{on}',on,mode,n,wal,limit,nt)
elif a.stage=='sampled-overhead':
 for rep in range(3):
  switches=['off','on'];random.Random(20261115+rep).shuffle(switches)
  for on in switches:run(f'sampled-overhead-r{rep}-query-{on}',on,'query',8192,0,9223372036854775807,16)
elif a.stage in ('representative','close-boundary'):
 n=json.loads((out/'selected-n.json').read_text())['n']
 cases=[('write',n,1,1048576,0),('write',n,0,65536,0),('query',8192,0,9223372036854775807,1),('query',8192,0,9223372036854775807,64)]
 for rep in range(3):
  order=cases.copy();random.Random(20261115+rep).shuffle(order)
  for mode,n,wal,limit,nt in order:run(f'{a.stage}-r{rep}-{mode}-w{wal}-t{nt}','on',mode,n,wal,limit,nt)
elif a.stage=='first':
 n=json.loads((out/'selected-n.json').read_text())['n']
 cases=[('write',n,wal,limit,0) for wal in (0,1) for limit in (16384,65536,1048576)]
 cases += [('query',8192,0,9223372036854775807,nt) for nt in (1,4,16,64)]
 for rep in range(3):
  order=cases.copy();random.Random(20261115+rep).shuffle(order)
  for mode,n,wal,limit,nt in order:run(f'first-r{rep}-{mode}-w{wal}-l{limit}-t{nt}','on',mode,n,wal,limit,nt)
