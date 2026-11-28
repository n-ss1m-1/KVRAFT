# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,subprocess,hashlib,shutil,difflib,os,time
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/compaction_rewrite';O=R/'benchmarks/results/compaction-rewrite-20261127'
def cmd(args,name):
 p=subprocess.run(args,cwd=R,capture_output=True);(O/name).write_bytes(p.stdout+p.stderr)
 if p.returncode:raise RuntimeError(str(args))