# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,tempfile,json,hashlib,shutil
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/group-commit-acceptance-20261121'
args=['c++','-O3','-DNDEBUG','-std=c++17','-pthread','-I'+str(o/'engine-snapshot/include'),str(r/'benchmarks/group_commit_acceptance/parameter_probe.cc'),str(o/'test-build/liblsm_engine.a'),'-o',str(o/'parameter-probe')]
p=subprocess.run(args,text=True,capture_output=True);(o/'probe-build.txt').write_text(p.stdout+p.stderr);p.check_returncode()
with (o/'commands.jsonl').open('a') as f:f.write(json.dumps(args)+'\n')
records=[]
for limit in [1,32,0,0]:
 with tempfile.TemporaryDirectory(prefix='lsm-group-parameter-') as d:
  args=[str(o/'parameter-probe'),d,str(limit)]
  try:
   p=subprocess.run(args,text=True,capture_output=True,timeout=3)
   record=dict(limit=limit,status='returned',returncode=p.returncode,stdout=p.stdout,stderr=p.stderr)
  except subprocess.TimeoutExpired as e:
   record=dict(limit=limit,status='timeout',seconds=3,stdout=(e.stdout or b'').decode(),stderr=(e.stderr or b'').decode())
  record['command']=args;records.append(record)
(o/'parameter-probes.json').write_text(json.dumps(records,indent=2))
assert records[0]['returncode']==0 and records[1]['returncode']==0
assert all(x['status']=='timeout' and 'put-start' in x['stderr'] and 'put-return' not in x['stderr'] for x in records[2:])
assert subprocess.check_output(['git','diff','--binary'],cwd=r)==(o/'worktree-before.patch').read_bytes()
hashes={str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((r/'src/lsm').rglob('*')) if p.is_file()}
assert hashes==json.loads((o/'engine-hashes.json').read_text())
(o/'git-status-after.txt').write_text(subprocess.check_output(['git','status','--short'],cwd=r,text=True))
shutil.copytree(r/'benchmarks/group_commit_acceptance',o/'tool-snapshot')
(o/'source-hashes.json').write_text(json.dumps({str(p.relative_to(o)):hashlib.sha256(p.read_bytes()).hexdigest() for base in ['engine-snapshot','tests-snapshot','test-support-snapshot','tool-snapshot'] for p in (o/base).rglob('*') if p.is_file()},indent=2))
print(json.dumps(records,indent=2))
