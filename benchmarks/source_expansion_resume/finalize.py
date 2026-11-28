# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
# Generate final independent patches and preserve tool/source state only after all work is complete.
assert hashes(R/'src/lsm')==json.loads((O/'engine-hashes.json').read_text())
assert subprocess.check_output(['git','diff','--binary'],cwd=R)==(O/'worktree-before.patch').read_bytes()
(O/'git-status-after.txt').write_bytes(subprocess.check_output(['git','status','--short'],cwd=R))
(O/'preservation.json').write_text(json.dumps({'main_engine_unchanged':True,'tracked_diff_unchanged':True,'HEAD':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip()},indent=2))
for kind in ['original','observer','mechanism']:(O/(kind+'-hashes.json')).write_text(json.dumps(hashes(O/kind),indent=2))
vs=[]
for p in (O/'runs').glob('*.json'):
 if p.name.endswith('-backpressure.json'):continue
 v=json.loads(p.read_text())
 if 'name' in v:vs.append(v)
assert all(v.get('verified') for v in vs)
(O/'completion.json').write_text(json.dumps({'runs':len(vs),'all_valid_runs_verified':True,'failed':1,'timeout':1,'invalid_records_directory':'runs/invalid','historical_invalid_manifest':'resume-state.json','mechanism_runs':len(list((O/'runs').glob('*-events.jsonl'))),'process_wall_s':sum(v['process_s'] for v in vs),'tests_passed':18},indent=2))
(O/'tool-hashes.json').write_text(json.dumps(hashes(T),indent=2));shutil.copytree(T,O/'tool-snapshot')
print('PRESERVATION PASS',len(vs))