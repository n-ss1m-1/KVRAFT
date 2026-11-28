# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import shutil,subprocess,json,hashlib
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-diagnosis-20261125';O.mkdir(exist_ok=False)
for a,n in [(['git','status','--short'],'git-status-before.txt'),(['git','diff','--binary'],'worktree-before.patch'),(['git','rev-parse','HEAD'],'HEAD.txt'),(['bash','-lc','uname -a; findmnt -T /home/truth/Code/my_kvraft; c++ --version; cmake --version'],'environment.txt')]:
 (O/n).write_bytes(subprocess.check_output(a,cwd=R))
shutil.copytree(R/'src/lsm',O/'original');shutil.copytree(R/'src/lsm',O/'instrumented')
(O/'hashes.json').write_text(json.dumps({str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((R/'src/lsm').rglob('*')) if p.is_file()},indent=2))
old=R/'benchmarks/results/compaction-acceptance-20261125';before=json.loads((old/'engine-hashes.json').read_text());now=json.loads((O/'hashes.json').read_text());(O/'history-source-comparison.json').write_text(json.dumps({'identical':before==now},indent=2));assert before==now,'source changed: examine before testing'
shutil.copy(R/'benchmarks/compaction_acceptance/bench.cc',O/'original-bench.cc')
print(O)
