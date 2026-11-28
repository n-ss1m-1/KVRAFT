# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
p=Path(__file__).with_name('run.py');s=p.read_text();s+="""
elif kind=='totalonly':
 for rep in range(3):
  cases=[0,1,4,16,64,256,1024]
  random.Random(20261415+rep).shuffle(cases)
  for b in cases:run(f'totalonly-r{rep}-0-{b}',0,b,timed=0)
""";p.write_text(s)
