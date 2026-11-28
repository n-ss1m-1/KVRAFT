# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,statistics,hashlib,subprocess,shutil
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-diagnosis-20261125';med=statistics.median
pos=json.loads((O/'hot-key-positions.json').read_text());print('positions',[(side,med(x['position_1based'] for x in pos if x['side']==side),sum(x['position_1based'] for x in pos if x['side']==side)/32) for side in ['before','after']])
for name in ['r0-write-s1-probe','r1-write-s1-probe','r2-write-s1-probe']:
 p=json.loads((O/'runs'/(name+'-slow-overlap.json')).read_text());print(name,'slowms',(p['slow_api']['end_ns']-p['slow_api']['start_ns'])/1e6,[(e['metric'],round(e['overlap_ns']/1e6,3)) for e in p['overlapping_events'] if e['metric'] in ['backpressure_cv','compact_task','merge_and_build_including_writer','output_write_sync_rename_dirsync','compact_publish_hold','old_file_cleanup','flush_task']])
for f in sorted((O/'runs').glob('r*-write-s1-plain.json')):
 p=json.loads(f.read_text());print(f.stem,p['requests']['max_ns']/1e6)
