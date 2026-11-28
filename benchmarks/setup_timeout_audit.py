# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import sys,json,subprocess,hashlib,datetime
r=Path("/home/truth/Code/my_kvraft");sys.path.insert(0,str(r/"benchmarks/source_expansion_resume"))
from paths import hashes
o=r/"benchmarks/results/source-expansion-timeout-audit-20261128";t=r/"benchmarks/source_expansion_timeout_audit"
assert not o.exists() and not t.exists();o.mkdir();t.mkdir()
old=r/"benchmarks/results/source-expansion-resume-20261128"
h=hashes(r/"src/lsm")
assert h==json.loads((old/"engine-hashes.json").read_text())
assert subprocess.check_output(["git","diff","--binary"],cwd=r)==(old/"worktree-before.patch").read_bytes()
(o/"engine-before.json").write_text(json.dumps(h,indent=2))
(o/"git-before.patch").write_bytes(subprocess.check_output(["git","diff","--binary"],cwd=r))
(o/"git-before.txt").write_bytes(subprocess.check_output(["git","status","--short"],cwd=r))
(o/"environment.txt").write_text(subprocess.check_output(["bash","-lc","date -Ins; uname -a; df -h / /mnt/d; cat /proc/sys/kernel/yama/ptrace_scope; command -v gdb; command -v strace; g++ --version; ps -eo pid,comm,args"],text=True))
(o/"snapshot-hashes.json").write_text(json.dumps({k:hashes(old/k) for k in ["original","observer","mechanism"]},indent=2))
(o/"binary-hash.txt").write_text(hashlib.sha256((old/"build/bench_mechanism").read_bytes()).hexdigest())
(t/"audit.cc").write_text("""
#define main old_benchmark_main
#include "/home/truth/Code/my_kvraft/benchmarks/source_expansion_resume/bench.cc"
#undef main
int main(int argc,char**argv){try{
 std::string mode=argv[1],d=argv[2];
 if(mode=="check"){
 size_t n=131072;Oracle oracle;std::vector<size_t> ids(n);std::iota(ids.begin(),ids.end(),0);std::mt19937_64 rng(20261115);std::shuffle(ids.begin(),ids.end(),rng);
 for(size_t i=0;i<n;++i){size_t id=ids[i];auto k=key(id);oracle[k]={{k,i+1,ValueType::kValue},value(id,0)};}
 fullcheck(d,oracle);std::cout<<"FULLCHECK PASS\n";return 0;}
 auto mf=Manifest::Load(d+"/MANIFEST");std::cout<<"{\"last\":"<<mf.LastSequence()<<",\"flushed\":"<<mf.FlushedSequence()<<",\"tables\":";layout(mf.Tables());std::cout<<"}\n";
 return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
""")
# Fix intentional C++ backslash literals via raw patch below in host writer.
(t/"CMakeLists.txt").write_text("""
cmake_minimum_required(VERSION 3.16)
project(timeout_audit LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
file(GLOB sources /home/truth/Code/my_kvraft/benchmarks/results/source-expansion-20261127/observer/lib/*.cc)
add_executable(audit audit.cc ${sources})
target_include_directories(audit PRIVATE /home/truth/Code/my_kvraft/benchmarks/results/source-expansion-20261127/observer/include /home/truth/Code/my_kvraft/benchmarks/results/source-expansion-20261127/observer/include/lsm /home/truth/Code/my_kvraft/benchmarks/source_expansion_resume)
target_compile_definitions(audit PRIVATE OBSERVE=1)
target_link_libraries(audit PRIVATE Threads::Threads)
""".replace("\$","$"))
print(o)