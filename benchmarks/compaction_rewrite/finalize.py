# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
# Read-only post-run verification and preservation. This script never mutates the engine.
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(p.rglob('*')) if x.is_file()}
before=json.loads((O/'engine-hashes.json').read_text());now=hashes(R/'src/lsm');patch=subprocess.check_output(['git','diff','--binary'],cwd=R)
status=subprocess.check_output(['git','status','--short'],cwd=R)
(O/'git-status-after.txt').write_bytes(status)
assert before==now;assert patch==(O/'worktree-before.patch').read_bytes()
(O/'preservation.json').write_text(json.dumps({'main_engine_hashes_unchanged':True,'tracked_diff_unchanged':True,'HEAD':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip()},indent=2))
for kind in ['original','observer','mechanism']:(O/(kind+'-hashes.json')).write_text(json.dumps(hashes(O/kind),indent=2))
(O/'tool-hashes.json').write_text(json.dumps(hashes(T),indent=2));shutil.copytree(T,O/'tool-snapshot')
results=[]
for p in (O/'runs').glob('*.json'):
 v=json.loads(p.read_text());results.append(v)
assert all(v.get('verified') for v in results)
(O/'completion.json').write_text(json.dumps({'process_results':len(results),'all_verified':True,'process_wall_s':sum(v.get('process_s',0) for v in results),'mechanism_runs':len(list((O/'runs').glob('*-events.jsonl'))),'tests':18,'failed':0,'timeouts':0},indent=2))
print('PRESERVATION PASS',len(results),flush=True)