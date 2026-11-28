# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,collections,statistics
O=Path('/home/truth/Code/my_kvraft/benchmarks/results/compaction-diagnosis-20261125')
for f in sorted((O/'runs').glob('r*.json')):
 if '.probe.' in f.name:continue
 v=json.loads(f.read_text());print(f.stem,round(v['passes'][1]['ops_s']) if v['mode']=='hot' else round(v['requests']['max_ns']/1e6,2))
f=O/'runs/r0-hot-s0-probe.probe.json'
if f.exists():
 for side in [0,1]:
  v=json.loads((O/f'runs/r0-hot-s{side}-probe.probe.json').read_text());print('HOT side',side)
  for c in v['cells']:
   if c['phase']==2: print(c['metric'],c['count'],c['units'],c['ns'],c['timed'])
for f in sorted((O/'runs').glob('r*-write-s1-probe.probe.json')):
 v=json.loads(f.read_text());print('WRITE',f.name)
 for c in v['cells']:
  if c['metric'] in ['backpressure_cv','first_db_lock','merge_and_build_including_writer','compact_publish_hold','flush_task','compact_task']:print(c)
