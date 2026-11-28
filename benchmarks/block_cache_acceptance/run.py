# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,struct,random,sys,hashlib
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/block-cache-acceptance-20261119';build=r/'build/block-cache-accept'
def run(name,nt,label,path,kind,n=8192,mech=False):
 cap=capacities[str(nt)][label];prefix=o/name;tool='cache-mechanism' if mech else 'cache-bench'
 args=[str(build/tool),str(o/f'fixtures/{nt}'),str(nt),str(cap),path,kind,str(n),str(prefix)]
 if Path(str(prefix)+'.jsonl').exists():raise RuntimeError('existing result')
 p=subprocess.run(args,text=True,capture_output=True,timeout=90)
 Path(str(prefix)+'.stderr.txt').write_text(p.stderr);Path(str(prefix)+'.jsonl').write_text(p.stdout)
 if p.returncode:raise RuntimeError((args,p.returncode,p.stderr))
 vs=[json.loads(s) for s in p.stdout.splitlines()]
 for v in vs:
  assert 0<=v['usage']<=cap,(name,v)
  if mech:assert v['cache_lookups']==v['hits']+v['misses'] and v['misses']==v['loads'] and v['pread_calls']==2*v['loads']
 with (o/'runs.jsonl').open('a') as f:
  for v in vs:f.write(json.dumps(dict(name=name,tables=nt,capacity_label=label,path=path,kind=kind,mechanism=mech,command=args,**v))+'\n')
 print(name,'passed',flush=True)
if sys.argv[1]=='prepare':
 (o/'fixtures').mkdir()
 capacities={};histograms={}
 for nt in [1,64]:
  subprocess.run([str(build/'cache-bench'),str(o/f'fixtures/{nt}'),str(nt),'0','generate','latest','1',str(o/'unused')],check=True)
  sizes=[]
  for file in (o/f'fixtures/{nt}').glob('*.sst'):
   b=file.read_bytes();off,size=struct.unpack_from('<QQ',b[-48:-4],28);idx=b[off:off+size];count=struct.unpack_from('<I',idx)[0];pos=4
   for i in range(count):
    ln=struct.unpack_from('<I',idx,pos)[0];pos+=4+ln+8+1+8;sizes.append(struct.unpack_from('<I',idx,pos)[0]);pos+=4
  total=sum(sizes);capacities[str(nt)]={'disabled':0,'full':total,'small':total//8};histograms[str(nt)]={str(s):sizes.count(s) for s in set(sizes)}
 (o/'capacities.json').write_text(json.dumps(dict(capacities=capacities,histograms=histograms),indent=2))
 for label in ['disabled','full','small']:run('pilot-'+label,64,label,'reader','uniform',512,True)
 sys.exit()
capacities=json.loads((o/'capacities.json').read_text())['capacities']
if sys.argv[1]=='formal':
 configs=[(nt,label,'reader',kind) for nt in [1,64] for label in ['disabled','full','small'] for kind in ['latest','oldest','missing','hot','uniform']]
 configs += [(64,label,'db',kind) for label in ['disabled','full','small'] for kind in ['oldest','hot','uniform']]
 for rep in range(3):
  cases=configs.copy();random.Random(20261115+rep).shuffle(cases)
  for i,(nt,label,path,kind) in enumerate(cases):run(f'formal-r{rep}-{nt}-{path}-{kind}-{label}',nt,label,path,kind)
elif sys.argv[1]=='mechanism':
 for nt in [1,64]:
  for label in ['disabled','full','small']:
   for kind in ['latest','oldest','missing','hot','uniform']:run(f'mechanism-{nt}-{kind}-{label}',nt,label,'reader',kind,mech=True)
 for label in ['disabled','full','small']:run('mechanism-db-'+label,64,label,'db','uniform',mech=True)
