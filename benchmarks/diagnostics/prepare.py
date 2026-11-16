from pathlib import Path
import shutil, subprocess, datetime
r=Path('/home/truth/Code/my_kvraft'); out=r/'benchmarks/results/diagnostics-20261116-first'
out.mkdir(exist_ok=False)
(out/'git-status-before.txt').write_text(subprocess.check_output(['git','status','--short'],cwd=r,text=True))
shutil.copytree(r/'src/lsm',out/'source-before/lsm')
shutil.copy2(r/'CMakeLists.txt',out/'source-before/CMakeLists.txt')
