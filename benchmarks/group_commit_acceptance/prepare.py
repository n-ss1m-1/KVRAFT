# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,shutil,json,hashlib
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/group-commit-acceptance-20261121'
o.mkdir(exist_ok=False)
def run(args,name,timeout=None):
 with (o/'commands.jsonl').open('a') as f:f.write(json.dumps(args)+'\n')
 p=subprocess.run(args,cwd=r,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
 (o/name).write_text(p.stdout)
 if p.returncode:raise RuntimeError((args,p.returncode))
 return p
run(['git','status','--short'],'git-status-before.txt')
run(['git','rev-parse','HEAD'],'git-head.txt')
run(['git','diff','--binary'],'worktree-before.patch')
for source,dest in [('src/lsm','engine-snapshot'),('tests/lsm','tests-snapshot'),('tests/kvraft','test-support-snapshot')]:
 shutil.copytree(r/source,o/dest)
shutil.copy(r/'CMakeLists.txt',o/'root-CMakeLists.txt')
(o/'engine-hashes.json').write_text(json.dumps({str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((r/'src/lsm').rglob('*')) if p.is_file()},indent=2))
run(['bash','-lc','uname -a; findmnt -T /home/truth/Code/my_kvraft; c++ --version; cmake --version'],'environment.txt')
# Standalone test CMake copied from root LSM section, avoiding unrelated dependencies/output paths.
root=(r/'CMakeLists.txt').read_text();section=root[root.index('add_library(lsm_engine STATIC'):];section=section.replace('${CMAKE_SOURCE_DIR}/src/lsm/include',str(o/'engine-snapshot/include')).replace('src/lsm/lib/',str(o/'engine-snapshot/lib')+'/').replace('tests/lsm/',str(o/'tests-snapshot')+'/').replace('${CMAKE_SOURCE_DIR}/tests/kvraft',str(o/'test-support-snapshot'))
(o/'test-project').mkdir()
(o/'test-project/CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(group_tests LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\nset(CMAKE_EXPORT_COMPILE_COMMANDS ON)\nfind_package(Threads REQUIRED)\n'+section)
run(['cmake','-S',str(o/'test-project'),'-B',str(o/'test-build'),'-DCMAKE_BUILD_TYPE=Release','-DLSM_DIAGNOSTICS=OFF'],'configure.txt')
run(['cmake','--build',str(o/'test-build'),'-j','4'],'build.txt')
run(['ctest','--test-dir',str(o/'test-build'),'-V'],'tests.txt')
run(['ctest','--test-dir',str(o/'test-build'),'-R','^lsm-group-commit$','--repeat','until-fail:3','-V'],'group-repeat.txt')
print(o,flush=True)
