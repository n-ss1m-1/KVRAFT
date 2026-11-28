# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
t=Path("/home/truth/Code/my_kvraft/benchmarks/source_expansion_resume")
p=t/"run.py";s=p.read_text();s=s.replace(" n=n or (524288", """ existing=O/'runs'/(name+'.json')
 if existing.exists():
  v=json.loads(existing.read_text());assert v.get('verified'),name
  print('SKIP',name,flush=True);return v
 free=shutil.disk_usage('/mnt/d').free
 if free<5*1024**3:raise RuntimeError('STOP: D free space below 5 GiB: '+str(free))
 print('DISK_FREE_GIB',round(free/1024**3,2),name,flush=True)
 n=n or (524288""");p.write_text(s)
(t/"resume.py").write_text("""from run import *
for r in range(6):
 for w in ['unique','update']:
  for f in ([1,2,4] if r%2==0 else [4,2,1]):run(f's2r{r}-4194304-{f}-{w}',4194304,f,w,'C')
for r in range(3):
 for f in ([1,2] if r%2==0 else [2,1]):run(f'walr{r}-4194304-{f}-update',4194304,f,'update','C',wal=1)
print('RESUMED C AND WAL COMPLETE',flush=True)
""")
(t/"all.py").write_text("""from paths import *
for script in ['resume.py','extras.py','keypairs.py','countercheck.py','check.py','summarize.py','compare.py','levels.py','cachecheck.py','finalize.py']:
 print('START',script,flush=True)
 with (O/(script+'.log')).open('w') as f:
  p=subprocess.run(['python3','-u',str(T/script)],stdout=f,stderr=subprocess.STDOUT)
 if p.returncode:raise RuntimeError('STOP '+script+' '+str(p.returncode))
 print('DONE',script,flush=True)
""")