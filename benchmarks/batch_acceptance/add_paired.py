# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
p=Path(__file__).with_name('run.py');s=p.read_text();s += """
elif kind=='paired':
 for rep in [3,4]:
  for w,b in ([(1,1),(1,0)] if rep==3 else [(1,0),(1,1)]):
   run(f'paired-r{rep}-{w}-{b}',w,b)
""";p.write_text(s)
