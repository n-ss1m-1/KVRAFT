# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
import shutil
# Run only after formal.py exits; build before sampling light observer.
T=R/'benchmarks/auto_compaction'
shutil.copytree(O/'engine',O/'observer')
p=O/'observer/include/lsm/db.h';s=p.read_text().replace('    bool failed_ = false;','    bool benchBusy_ = false;\n    bool failed_ = false;');p.write_text(s)
p=O/'observer/lib/db.cc';s=p.read_text();a=s.index('void DB::CompactionLoop()');b=s.index('void DB::RotateMemTableLocked()',a);part=s[a:b].replace('            }\n\n            // 2.','                benchBusy_=true;\n            }\n\n            // 2.').replace('            CompactTables(taskRequest, sourceLevel);','            CompactTables(taskRequest, sourceLevel);\n            {std::lock_guard<std::mutex> lock(dbMutex_);benchBusy_=false;}\n            backgroundCv_.notify_all();');p.write_text(s[:a]+part+s[b:])
s=(T/'bench.cc').read_text().replace('#ifdef PROBE\n static void settle','#if defined(PROBE) || defined(OBSERVE)\n static void settle').replace('#ifdef PROBE\nif(converge)','#if defined(PROBE) || defined(OBSERVE)\nif(converge)')
(T/'observer.cc').write_text(s)
# standalone observer build, same source/compiler switches, count-free.
P=O/'observer-project';P.mkdir();(P/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(observer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
file(GLOB sources {O}/observer/lib/*.cc)
add_executable(bench_observer {T}/observer.cc ${{sources}})
target_include_directories(bench_observer PRIVATE {O}/observer/include {O}/observer/include/lsm {T})
target_compile_definitions(bench_observer PRIVATE OBSERVE=1)
target_link_libraries(bench_observer PRIVATE Threads::Threads)
''')
for cmd,name in [(['cmake','-S',str(P),'-B',str(O/'observer-build'),'-DCMAKE_BUILD_TYPE=Release'],'configure-observer.txt'),(['cmake','--build',str(O/'observer-build'),'-j4'],'build-observer.txt')]:
 p=subprocess.run(cmd,capture_output=True);(O/name).write_bytes(p.stdout+p.stderr);assert p.returncode==0
# run() locates executables under build
shutil.copy(O/'observer-build/bench_observer',O/'build/bench_observer')
for r in range(3):
 for work in ['unique','update']:
  for a in ([0,1] if r%2==0 else [1,0]):run(f'o{r}-{work}-a{a}','observer','settle',a,0,work)
# direct off/on observer throughput overhead versus core is reported with sampling variability.
print('OBSERVER DONE',flush=True)