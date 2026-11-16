from pathlib import Path
import json,subprocess,time,random,sys
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/diagnostics-20261116-followup';(o/'databases').mkdir(exist_ok=True)
def run(name,on,wal,limit,threads,n=16384,query=False):
 prefix=o/name
 if query:args=[str(r/f'build/diag-{on}/diagnose'),str(o/'databases'/name),'query','8192','0','9223372036854775807','64','20261115',str(prefix)]
 else:args=[str(r/f'build/diag-{on}/followup-write'),str(o/'databases'/name),str(n),str(wal),str(limit),str(threads),str(prefix)]
 if Path(str(prefix)+'.stdout.json').exists():raise RuntimeError('existing result')
 b=time.monotonic()
 try:p=subprocess.run(args,capture_output=True,text=True,timeout=240)
 except subprocess.TimeoutExpired:
  (o/(name+'.timeout.json')).write_text(json.dumps(dict(command=args,timeout=240)));raise
 (o/(name+'.stdout.json')).write_text(p.stdout);(o/(name+'.stderr.txt')).write_text(p.stderr)
 if p.returncode:raise RuntimeError((args,p.returncode,p.stderr))
 v=json.loads(p.stdout);v.update(name=name,on=on,wal=wal,limit=limit,threads=threads,query=query,process_seconds=time.monotonic()-b,command=args)
 if on=='on':
  stats=json.loads(Path(str(prefix)+'.stats.json').read_text());assert not stats['dropped_threads'] and not stats['tasks_dropped']
  def count(metric,phase=1,op=1):return sum(c['count'] for c in stats['cells'] if c['metric']==metric and c['phase']==phase and c['op']==op)
  if not query:
   assert count('memtable_add')==n and count('wal_sync')==(n if wal else 0)
   assert count('first_lock')==n
   assert len(stats['tasks'])==v['tables']
   v['automatic_flushes']=count('rotate')
   assert v['automatic_flushes']>=3 or name.startswith('pilot')
  else:
   for phase in range(6,12):
    def cnt(m):return count(m,phase,3)
    def units(m):return sum(c['units'] for c in stats['cells'] if c['metric']==m and c['phase']==phase and c['op']==3)
    assert cnt('bloom_checks')-cnt('bloom_rejects')==cnt('bloom_pass_found')+cnt('bloom_pass_not_found')
    assert cnt('blocks')==units('found_table_blocks')+units('not_found_table_blocks')
    assert cnt('blocks')==cnt('bloom_pass_found')+cnt('bloom_pass_not_found')+units('same_table_extra_blocks')
 with (o/'runs.jsonl').open('a') as f:f.write(json.dumps(v)+'\n')
 print(name,round(v['process_seconds'],3),'s',flush=True)
if sys.argv[1]=='pilot':
 run('pilot-off-64k','on',0,65536,1)
 run('pilot-on-4threads','on',1,1048576,4,1024)
 run('pilot-query','on',0,0,1,query=True)
elif sys.argv[1]=='overhead':
 for rep in range(3):
  for label,wal,limit,threads,query in [('off64',0,65536,1,False),('on4',1,1048576,4,False),('query',0,0,1,True)]:
   switches=['on','off'];random.Random(20261115+rep).shuffle(switches)
   for on in switches:run(f'overhead-r{rep}-{label}-{on}',on,wal,limit,threads,query=query)
elif sys.argv[1]=='formal':
 configs=[('off64',0,65536,1,False),('off1m',0,1048576,1,False),('on1',1,1048576,1,False),('on2',1,1048576,2,False),('on4',1,1048576,4,False),('query',0,0,1,True)]
 for rep in range(3):
  order=configs.copy();random.Random(20261115+rep).shuffle(order)
  for label,wal,limit,threads,query in order:
   switches=['on','off'] if wal else ['on'];random.Random(20261115+rep+threads).shuffle(switches)
   for switch in switches:run(f'formal-r{rep}-{label}-{switch}',switch,wal,limit,threads,query=query)
  p=subprocess.run([str(r/'build/diag-off/crc-bench')],text=True,capture_output=True,check=True);(o/f'crc-r{rep}.jsonl').write_text(p.stdout)
