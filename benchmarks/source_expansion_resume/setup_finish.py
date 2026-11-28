# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import shutil
t=Path("/home/truth/Code/my_kvraft/benchmarks/source_expansion_resume");o=t.parent/"results/source-expansion-resume-20261128"
bad="matrixm-262144-2-unique"
(o/"runs/invalid").mkdir(exist_ok=True)
for p in (o/"runs").glob(bad+"*"):
 if p.is_file():shutil.move(str(p),str(o/"runs/invalid"/p.name))
p=t/"finalize.py";s=p.read_text().replace("shutil.copytree(T,O/'tool-snapshot')","shutil.copytree(T,O/'tool-snapshot')");p.write_text(s)
(t/"finish_sampling.py").write_text("""from run import *
# Complete key formal pairs before extra diagnostics; no retry of timed-out detailed variant.
exec((T/'keypairs.py').read_text())
configs=[(1048576,2),(4194304,2),(4194304,4)]
for b,f in configs:
 for w in ['unique','update']:run(f'matrixm-{b}-{f}-{w}',b,f,w,kind='mechanism')
for r in range(2):
 for w in ['unique','update']:
  for b,f in ([(1048576,1),(1048576,4),(4194304,1),(4194304,4)] if r%2==0 else [(4194304,4),(4194304,1),(1048576,4),(1048576,1)]):
   run(f'm{r}-{b}-{f}-{w}',b,f,w,kind='mechanism')
 for w in ['unique','update']:
  for f in ([1,2,4] if r%2==0 else [4,2,1]):run(f'cm{r}-4194304-{f}-{w}',4194304,f,w,'C',kind='mechanism')
for r in range(3):
 for kind in (['observer','mechanism'] if r%2==0 else ['mechanism','observer']):run(f'overhead{r}-{kind}',1048576,4,'unique',kind=kind)
# Retain the existing limited query design.
s=(T/'extras.py').read_text()
exec(s[s.index('for r in range(3):\n for scale,cs'):])
""")
(t/"finish.py").write_text("""from paths import *
for script in ['finish_sampling.py','countercheck.py','check.py','summarize.py','compare.py','levels.py','cachecheck.py','strata.py']:
 print('START',script,flush=True)
 with (O/(script+'.log')).open('w') as f:p=subprocess.run(['python3','-u',str(T/script)],stdout=f,stderr=subprocess.STDOUT)
 if p.returncode:raise RuntimeError('STOP '+script+' '+str(p.returncode))
 print('DONE',script,flush=True)
""")