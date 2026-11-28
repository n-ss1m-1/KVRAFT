# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,struct,csv,statistics
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/dual-background-20261125';F=R/'benchmarks/results/compaction-acceptance-20261125/fixtures/multi-64'
# Header magic8 + version/header/payload u32, generation u64 at20.
base=struct.unpack_from('<Q',(F/'MANIFEST').read_bytes(),20)[0]
rows=[]
for f in (O/'runs').glob('*.json'):
 if f.name.endswith('.probe.json'):continue
 v=json.loads(f.read_text());b=(O/'copies'/v['name']/'MANIFEST').read_bytes();generation=struct.unpack_from('<Q',b,20)[0];delta=generation-base
 # Each successful Flush and Compact with >1 input publishes one MANIFEST.
 compact_count=int(v['comp']);flushes=delta-compact_count
 rows.append({'run':v['name'],'manifest_generation_delta':delta,'compaction_publications':compact_count,'flush_publications':flushes})
(O/'publication-counts.json').write_text(json.dumps(rows,indent=2));print('publication count records',len(rows),'baseline generation',base)
