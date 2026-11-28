# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,subprocess,hashlib,shutil,difflib,os,time
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/source_expansion';O=R/'benchmarks/results/source-expansion-20261127';P=R/'benchmarks/compaction_rewrite'
def cmd(a,n):
 p=subprocess.run(a,cwd=R,capture_output=True);(O/n).write_bytes(p.stdout+p.stderr);assert p.returncode==0,(a,n)
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(p.rglob('*')) if x.is_file()}