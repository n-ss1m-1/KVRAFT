# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,hashlib,platform
r=Path('/home/truth/Code/my_kvraft')
o=r/'benchmarks/results/batch-acceptance-20261120'
o.mkdir(exist_ok=False)
def cmd(args,name):
 p=subprocess.run(args,cwd=r,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (o/name).write_text(p.stdout)
 if p.returncode:raise RuntimeError((args,p.returncode))
 return p.stdout
cmd(['git','status','--short'],'git-status-before.txt')
cmd(['git','rev-parse','HEAD'],'git-head.txt')
cmd(['git','diff','--binary'],'worktree-before.patch')
import shutil
shutil.copytree(r/'src/lsm',o/'engine-snapshot')
shutil.copytree(r/'tests/lsm',o/'tests-snapshot')
shutil.copytree(r/'tests/kvraft',o/'test-support-snapshot')
shutil.copy(r/'CMakeLists.txt',o/'root-CMakeLists.txt')
hashes={str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((r/'src/lsm').rglob('*')) if p.is_file()}
(o/'engine-hashes.json').write_text(json.dumps(hashes,indent=2))
cmd(['bash','-lc','uname -a; findmnt -T /home/truth/Code/my_kvraft; lscpu; free -h; c++ --version; cmake --version'],'environment.txt')
# Dedicated test build uses the root CMake targets, preserving the previous build.
cmd(['cmake','-S',str(r),'-B',str(o/'test-build'),'-DCMAKE_BUILD_TYPE=Release','-DLSM_DIAGNOSTICS=OFF'],'test-configure.txt')
targets=['test-lsm-'+n for n in ['manifest','db','internal-key','skip-list','mem-table','bloom-filter','data-block','coding','wal-reader','wal-writer','sstable-builder','sstable-reader','sstable-writer','block-cache','write-batch']]
cmd(['cmake','--build',str(o/'test-build'),'-j','4','--target',*targets],'test-build.txt')
cmd(['ctest','--test-dir',str(o/'test-build'),'-V'],'tests.txt')
cmd(['cmake','-S',str(r/'benchmarks/batch_acceptance'),'-B',str(o/'build'),'-DCMAKE_BUILD_TYPE=Release','-DENGINE_ROOT='+str(o/'engine-snapshot')],'configure.txt')
cmd(['cmake','--build',str(o/'build'),'-j','4'],'build.txt')
(o/'manifest.json').write_text(json.dumps(dict(seed=20261115,n=16384,key_bytes=16,value_bytes=128,memtable=1048576,wal_sync='each successful Write',diagnostics=False,standard=17),indent=2))
print(o)
