# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,hashlib,subprocess,shutil
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/write_tail_diagnosis';O=R/'benchmarks/results/write-tail-diagnosis-20261125'
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(p.rglob('*')) if x.is_file()}
before=json.loads((O/'engine-hashes.json').read_text());now=hashes(R/'src/lsm');patch=subprocess.check_output(['git','diff','--binary'],cwd=R)
check=dict(engine_unchanged=before==now,tracked_diff_unchanged=patch==(O/'worktree-before.patch').read_bytes())
(O/'preservation.json').write_text(json.dumps(check,indent=2));assert all(check.values())
(O/'worktree-after.patch').write_bytes(patch);(O/'git-status-after.txt').write_bytes(subprocess.check_output(['git','status','--short'],cwd=R))
(O/'probe.patch').write_bytes(subprocess.run(['diff','-ruN',str(O/'engine-snapshot'),str(O/'probe')],stdout=subprocess.PIPE).stdout)
(O/'variant-hashes.json').write_text(json.dumps({k:hashes(O/k) for k in ['engine-snapshot','probe']},indent=2))
shutil.copytree(T,O/'tools-snapshot',dirs_exist_ok=True)
with (O/'tools.patch').open('wb') as f:
 for p in sorted(T.iterdir()):
  if p.is_file():f.write(subprocess.run(['git','diff','--no-index','--','/dev/null',str(p)],stdout=subprocess.PIPE).stdout)
(O/'fixture-hashes.json').write_text(json.dumps(hashes(R/'benchmarks/results/compaction-acceptance-20261125/fixtures/multi-64'),indent=2))
probes=[json.loads(p.read_text()) for p in (O/'runs').glob('*.probe.json')]
(O/'diagnostic-integrity.json').write_text(json.dumps(dict(processes=len(probes),max_slots=max(p['slots'] for p in probes),overflows=sum(p['overflow'] for p in probes),dropped=sum(p['events_dropped'] for p in probes)),indent=2))
print(check)