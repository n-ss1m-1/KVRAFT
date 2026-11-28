# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
# Compare formal observer's light counters to a count-free current snapshot using a dedicated executable.
# Run only after all formal/mechanism sampling; build then run serially.
p=O/'counter-free';shutil.copytree(O/'observer',p)
q=p/'lib/db.cc';s=q.read_text();s=s.replace('    totals::flushBytes+=fileSize;totals::flushCount++;','').replace('            totals::compBytes+=currentMeta->fileSize;totals::compCount++;','').replace('        totals::compBytes+=currentMeta->fileSize;totals::compCount++;','');q.write_text(s)
proj=O/'counter-project';proj.mkdir();(proj/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(countercheck LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
file(GLOB sources {p}/lib/*.cc)
add_executable(counterfree {T}/bench.cc ${{sources}})
target_include_directories(counterfree PRIVATE {p}/include {p}/include/lsm {T})
target_compile_definitions(counterfree PRIVATE OBSERVE=1)
target_link_libraries(counterfree PRIVATE Threads::Threads)
''')
cmd(['cmake','-S',str(proj),'-B',str(O/'counter-build'),'-DCMAKE_BUILD_TYPE=Release'],'counter-configure.txt');cmd(['cmake','--build',str(O/'counter-build'),'-j4'],'counter-build.txt');shutil.copy(O/'counter-build/counterfree',O/'build/bench_counterfree')
from run import run
for r in range(3):
 for kind in (['observer','counterfree'] if r%2==0 else ['counterfree','observer']):run(f'counter{r}-{kind}',1048576,4,'unique',kind=kind)
print('COUNTER OVERHEAD COMPLETE',flush=True)