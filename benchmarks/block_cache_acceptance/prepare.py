# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import shutil,subprocess
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/block-cache-acceptance-20261119';o.mkdir(exist_ok=False)
shutil.copytree(r/'src/lsm',o/'engine-snapshot')
(o/'git-before.txt').write_text(subprocess.check_output(['git','status','--short'],cwd=r,text=True))
