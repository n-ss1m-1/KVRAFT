# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,hashlib,shutil
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/auto_compaction';O=R/'benchmarks/results/auto-compaction-20261127'
O.mkdir(exist_ok=False)
def cmd(a,n):
 p=subprocess.run(a,cwd=R,capture_output=True);(O/n).write_bytes(p.stdout+p.stderr)
 if p.returncode:raise RuntimeError(a)
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(p.rglob('*')) if x.is_file()}
cmd(['git','status','--short'],'git-status-before.txt');cmd(['git','diff','--binary'],'worktree-before.patch');cmd(['git','rev-parse','HEAD'],'HEAD.txt')
(O/'engine-hashes.json').write_text(json.dumps(hashes(R/'src/lsm'),indent=2))
for a,b in [('src/lsm','engine'),('tests/lsm','tests'),('tests/kvraft','support')]:shutil.copytree(R/a,O/b)
shutil.copy(R/'CMakeLists.txt',O/'root-CMakeLists.txt')
s=(R/'CMakeLists.txt').read_text();s=s[s.index('add_library(lsm_engine STATIC'):].replace('${CMAKE_SOURCE_DIR}/src/lsm/include',str(O/'engine/include')).replace('src/lsm/lib/',str(O/'engine/lib')+'/').replace('tests/lsm/',str(O/'tests')+'/').replace('${CMAKE_SOURCE_DIR}/tests/kvraft',str(O/'support'))
(O/'test-project').mkdir();(O/'test-project/CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(t LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\nfind_package(Threads REQUIRED)\n'+s)
cmd(['bash','-lc','uname -a; findmnt -T /home/truth/Code/my_kvraft; c++ --version; cmake --version; ps -eo pid,comm,args'],'environment.txt')
cmd(['cmake','-S',str(O/'test-project'),'-B',str(O/'test-build'),'-DCMAKE_BUILD_TYPE=Release','-DLSM_DIAGNOSTICS=OFF'],'configure-tests.txt')
cmd(['cmake','--build',str(O/'test-build'),'-j4'],'build-tests.txt')
cmd(['ctest','--test-dir',str(O/'test-build'),'-V'],'tests.txt')
print('TESTS PASS',flush=True)