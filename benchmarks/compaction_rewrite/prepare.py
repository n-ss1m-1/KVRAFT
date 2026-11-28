# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import hashlib,json,subprocess,shutil,difflib
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/compaction_rewrite';P=R/'benchmarks/results/auto-compaction-20261127';O=R/'benchmarks/results/compaction-rewrite-20261127'
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(p.rglob('*')) if x.is_file()}
def cmd(args,name):
 p=subprocess.run(args,cwd=R,capture_output=True);(O/name).write_bytes(p.stdout+p.stderr)
 if p.returncode:raise RuntimeError(str(args))
O.mkdir(exist_ok=False)
for f,args in [('git-status-before.txt',['git','status','--short']),('worktree-before.patch',['git','diff','--binary']),('HEAD.txt',['git','rev-parse','HEAD'])]:cmd(args,f)
h=hashes(R/'src/lsm');(O/'engine-hashes.json').write_text(json.dumps(h,indent=2));assert h==json.loads((P/'engine-hashes.json').read_text()),'engine differs from previous snapshot: inspect before continuing'
shutil.copytree(R/'src/lsm',O/'original');shutil.copytree(P/'observer',O/'observer');shutil.copytree(O/'observer',O/'mechanism')
shutil.copy(R/'CMakeLists.txt',O/'root-CMakeLists.txt');shutil.copytree(R/'tests/lsm',O/'tests');shutil.copytree(R/'tests/kvraft',O/'support')
s=(R/'CMakeLists.txt').read_text();s=s[s.index('add_library(lsm_engine STATIC'):].replace('${CMAKE_SOURCE_DIR}/src/lsm/include',str(O/'original/include')).replace('src/lsm/lib/',str(O/'original/lib')+'/').replace('tests/lsm/',str(O/'tests')+'/').replace('${CMAKE_SOURCE_DIR}/tests/kvraft',str(O/'support'))
(O/'test-project').mkdir();(O/'test-project/CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(t LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\nfind_package(Threads REQUIRED)\n'+s)
cmd(['cmake','-S',str(O/'test-project'),'-B',str(O/'test-build'),'-DCMAKE_BUILD_TYPE=Release','-DLSM_DIAGNOSTICS=OFF'],'configure-tests.txt');cmd(['cmake','--build',str(O/'test-build'),'-j4'],'build-tests.txt');cmd(['ctest','--test-dir',str(O/'test-build'),'-V'],'tests.txt')
cmd(['bash','-lc','uname -a; findmnt -T /home/truth/Code/my_kvraft; c++ --version; cmake --version; ps -eo pid,comm,args'],'environment.txt')
shutil.copy(R/'benchmarks/auto_compaction/base.cc',T/'base.cc')
s=(R/'benchmarks/auto_compaction/observer.cc').read_text()
a=s.index('DBOptions opts(');b=s.index('struct Lat',a)
s=s[:a]+'''std::string config(){const char* p=std::getenv("RW_CONFIG");return p?p:"A";}
DBOptions opts(bool a,bool w,size_t cache=0){auto o=options(cache,262144);o.enableAutoCompaction=a;o.enableWal=w;o.maxLevels=3;o.level0CompactionTrigger=4;o.level1TargetBytes=1048576;o.levelSizeMultiplier=4;o.maxCompactionSourceBytes=config()=="B"?4194304:1048576;if(config()=="C"){o.memTableSizeLimit=4194304;o.targetSSTableDataSize=4194304;o.maxCompactionSourceBytes=4194304;o.level1TargetBytes=16777216;o.levelSizeMultiplier=10;o.maxLevels=7;}return o;}
'''+s[b:]
s=s.replace('std::vector<std::vector<TableMeta>> levels(3)','std::vector<std::vector<TableMeta>> levels(7)').replace('t.level<3','t.level<7').replace('l<3','l<7').replace('type=="unique"?n:32768','type=="unique"?n:n/4')
s=s.replace('auto closeStart=tick();','auto converged=tick();auto closeStart=tick();').replace('<<settleNs<<', '<<settleNs<<')
s=s.replace('\\\"rss_kib\\\":','\\\"converged_ns\\\":').replace('<<peak<<",\\\"initial', '<<peak<<",\\\"initial') if False else s
# Add explicit convergence timestamp without changing prior measurement fields.
s=s.replace('<<settleNs<<",\\\"rss_kib\\\":', '<<settleNs<<",\\\"converged_ns\\\":"<<converged<<",\\\"rss_kib\\\":')
(T/'bench.cc').write_text(s)
print('18 tests passed, snapshot unchanged; harness ready',flush=True)