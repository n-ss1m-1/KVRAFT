# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
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
exec('for r in range(3):'+chr(10)+' for scale,cs'+s.split(' for scale,cs',1)[1])
