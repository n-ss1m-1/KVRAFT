# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
for script in ['finish_sampling.py','countercheck.py','check.py','summarize.py','compare.py','levels.py','cachecheck.py','strata.py']:
 print('START',script,flush=True)
 with (O/(script+'.log')).open('w') as f:p=subprocess.run(['python3','-u',str(T/script)],stdout=f,stderr=subprocess.STDOUT)
 if p.returncode:raise RuntimeError('STOP '+script+' '+str(p.returncode))
 print('DONE',script,flush=True)
