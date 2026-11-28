# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,hashlib,subprocess,shutil
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/auto_compaction';O=R/'benchmarks/results/auto-compaction-20261127'
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(p.rglob('*')) if x.is_file()}
checks=dict(engine_unchanged=hashes(R/'src/lsm')==json.loads((O/'engine-hashes.json').read_text()),tracked_diff_unchanged=subprocess.check_output(['git','diff','--binary'],cwd=R)==(O/'worktree-before.patch').read_bytes())
(O/'preservation.json').write_text(json.dumps(checks,indent=2));assert all(checks.values())
(O/'engine-access.patch').write_bytes(subprocess.run(['diff','-ruN',str(R/'src/lsm'),str(O/'engine')],capture_output=True).stdout)
(O/'probe.patch').write_bytes(subprocess.run(['diff','-ruN',str(O/'engine'),str(O/'probe')],capture_output=True).stdout)
(O/'variant-hashes.json').write_text(json.dumps({k:hashes(O/k) for k in ['engine','probe','observer']},indent=2))
(O/'observer.patch').write_bytes(subprocess.run(['diff','-ruN',str(O/'engine'),str(O/'observer')],capture_output=True).stdout)
shutil.copytree(T,O/'tools-snapshot',dirs_exist_ok=True)
with (O/'tools.patch').open('wb') as f:
 for p in sorted(T.iterdir()):
  if p.is_file():f.write(subprocess.run(['git','diff','--no-index','--','/dev/null',str(p)],capture_output=True).stdout)
vs=[json.loads(p.read_text()) for p in (O/'runs').glob('*.json')]
(O/'completion.json').write_text(json.dumps(dict(processes=len(vs),all_verified=all(v.get('verified') for v in vs),process_wall_s=sum(v['process_s'] for v in vs),diagnostic_dropped=sum(v.get('mechanism',{}).get('dropped',0) for v in vs)),indent=2))
print(checks)