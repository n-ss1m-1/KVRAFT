# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,shutil,json,hashlib,csv
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/write_tail_diagnosis';O=R/'benchmarks/results/write-tail-diagnosis-20261125';H=R/'benchmarks/results/dual-background-20261125'
O.mkdir(exist_ok=False)
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(p.rglob('*')) if x.is_file()}
def cmd(a,n):
 p=subprocess.run(a,cwd=R,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(O/n).write_bytes(p.stdout)
 if p.returncode:raise RuntimeError(a)
cmd(['git','status','--short'],'git-status-before.txt');cmd(['git','diff','--binary'],'worktree-before.patch');cmd(['git','rev-parse','HEAD'],'git-head.txt')
(O/'engine-hashes.json').write_text(json.dumps(hashes(R/'src/lsm'),indent=2));shutil.copytree(R/'src/lsm',O/'engine-snapshot')
assert hashes(O/'engine-snapshot')==hashes(H/'engine-snapshot'),'current engine differs; stop for review'
shutil.copytree(H/'engine-snapshot-probe',O/'probe')
for n in ['base.cc','bench.cc','dd.h']:shutil.copy(R/'benchmarks/dual_background'/n,T/n)
rows=[]
for p in sorted((H/'runs').glob('r*-write-off-c*-engine-snapshot.json')):
 v=json.loads(p.read_text()); data=list(csv.DictReader(p.with_name(p.stem+'-timeline.csv').open()))
 slow=[dict(batch=i,start_ms=int(x['start_ns'])/1e6,elapsed_ms=int(x['elapsed_ns'])/1e6) for i,x in enumerate(data) if int(x['elapsed_ns'])>100000000]
 rows.append(dict(run=p.stem,max_ms=v['requests']['max_ns']/1e6,slow=slow,compact_ms=v['compact_ns']/1e6))
(O/'historical-anomalies.json').write_text(json.dumps(rows,indent=2))
cmd(['bash','-lc','uname -a; findmnt -T /home/truth/Code/my_kvraft; c++ --version; cmake --version; command -v perf; command -v strace; cat /proc/sys/kernel/perf_event_paranoid; ps -eo pid,comm,args'],'environment.txt')
print(json.dumps([x for x in rows if x['slow']],indent=2))