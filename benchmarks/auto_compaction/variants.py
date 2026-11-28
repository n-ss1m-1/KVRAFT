# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import shutil,subprocess
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/auto_compaction';O=R/'benchmarks/results/auto-compaction-20261127'
# Friend access changes visibility only in isolated headers; no production API modification.
p=O/'engine/include/lsm/db.h';s=p.read_text().replace('class DB : private lsm::NonCopyable\n{','class DB : private lsm::NonCopyable\n{\n friend struct BenchAccess;');p.write_text(s)
shutil.copytree(O/'engine',O/'probe')
p=O/'probe/include/lsm/db.h';s=p.read_text().replace('    bool failed_ = false;','    bool benchBusy_ = false;\n    bool failed_ = false;');p.write_text(s)
p=O/'probe/lib/db.cc';s=p.read_text();s=s.replace('            // 2. 解锁执行  ! 注意这里不能再访问immutable_、compactionRequested_、compactionCompleted_','            // 2. 解锁执行  ! 注意这里不能再访问immutable_、compactionRequested_、compactionCompleted_')
# Set busy at claim while lock held; reset after CompactTables returns (including cleanup).
s=s.replace('                    sourceLevel = selected.value();','                    sourceLevel = selected.value();').replace('            CompactTables(taskRequest, sourceLevel);','            CompactTables(taskRequest, sourceLevel);\n            {std::lock_guard<std::mutex> lock(dbMutex_);benchBusy_=false;}\n            backgroundCv_.notify_all();')
a=s.index('void DB::CompactionLoop()');b=s.index('void DB::RotateMemTableLocked()',a);part=s[a:b];part=part.replace('            }\n\n            // 2.', '                benchBusy_=true;\n            }\n\n            // 2.');s=s[:a]+part+s[b:]
s=s.replace('void DB::CompactTables(uint64_t taskRequest, uint32_t sourceLevel)\n{','void DB::CompactTables(uint64_t taskRequest, uint32_t sourceLevel)\n{\n ap::Task task(sourceLevel);')
s=s.replace('    // 2. 锁外：BuildCompactionOutputs()','    for(auto& m:inputMetas){task.input_bytes+=m.fileSize;}\n    task.source=sourceInputs.size();task.target=targetInputs.size();\n    for(auto* r:inputs)for(auto it=r->Begin();it.Valid();it.Next())++task.input_records;\n    // 2. 锁外：BuildCompactionOutputs()')
s=s.replace('    // 3. 锁外：准备新的SSTableState','    for(auto& m:outputMetas){task.output_bytes+=m.fileSize;task.outputs++;SSTableReader r(TablePath(m.fileNumber));for(auto it=r.Begin();it.Valid();it.Next())++task.output_records;}\n    // 3. 锁外：准备新的SSTableState')
s=s.replace('        levels_.swap(candidateLevels);','        levels_.swap(candidateLevels);\n        task.published=ap::now();')
s=s.replace('    uint64_t fileSize = LSM_TIME(Writer, SSTableWriter::Write(sstablePath, builder));','    uint64_t fileSize = LSM_TIME(Writer, SSTableWriter::Write(sstablePath, builder));\n    ap::flushBytes+=fileSize;ap::flushCount++;')
s=s.replace('            LSM_TIME(Backpressure, backgroundCv_.wait','            auto ap_start=ap::now();\n            LSM_TIME(Backpressure, backgroundCv_.wait').replace('            CheckHealthLocked();\n            continue;', '            ap::bpNs+=ap::now()-ap_start;ap::bpCount++;\n            CheckHealthLocked();\n            continue;')
s=s.replace('lsm_diag::count(lsm_diag::Tables);','lsm_diag::count(lsm_diag::Tables);ap::tables++;')
p.write_text(s)
s='''#pragma once
#include <chrono>
#include <cstdint>
#include <fstream>
namespace ap {
inline uint64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Event{uint64_t start=0,end=0,published=0,input_bytes=0,output_bytes=0,input_records=0,output_records=0,source=0,target=0,outputs=0;uint32_t level=0;};
inline Event events[4096];inline unsigned used=0,dropped=0;
inline uint64_t flushBytes=0,flushCount=0;inline thread_local uint64_t bpNs=0,bpCount=0,tables=0;
struct Task:Event{Task(uint32_t l){level=l;start=now();}~Task(){end=now();if(used<4096)events[used++]=*this;else dropped++;}};
inline void dump(const std::string& path){std::ofstream f(path);f<<"level,start_ns,published_ns,end_ns,source,target,outputs,input_bytes,output_bytes,input_records,output_records\\n";for(unsigned i=0;i<used;i++){auto&e=events[i];f<<e.level<<','<<e.start<<','<<e.published<<','<<e.end<<','<<e.source<<','<<e.target<<','<<e.outputs<<','<<e.input_bytes<<','<<e.output_bytes<<','<<e.input_records<<','<<e.output_records<<'\\n';}}
}
''';(T/'ap.h').write_text(s)
s='''cmake_minimum_required(VERSION 3.16)
project(auto_bench LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
option(LSM_DIAGNOSTICS "must be OFF" OFF)
if(LSM_DIAGNOSTICS)
message(FATAL_ERROR "diagnostics must be off")
endif()
foreach(kind engine probe)
file(GLOB sources ${RESULT_ROOT}/${kind}/lib/*.cc)
add_library(e_${kind} STATIC ${sources})
target_include_directories(e_${kind} PUBLIC ${RESULT_ROOT}/${kind}/include PRIVATE ${RESULT_ROOT}/${kind}/include/lsm ${CMAKE_CURRENT_SOURCE_DIR})
if(kind STREQUAL "probe")
target_compile_definitions(e_${kind} PUBLIC PROBE=1)
target_compile_options(e_${kind} PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/ap.h)
endif()
add_executable(bench_${kind} bench.cc)
target_link_libraries(bench_${kind} PRIVATE e_${kind} Threads::Threads)
endforeach()
''';(T/'CMakeLists.txt').write_text(s)
# reuse only common helpers predating Compact API
s=(R/'benchmarks/compaction_acceptance/bench.cc').read_text();s=s[:s.index('void create(')];(T/'base.cc').write_text(s)
print('variants ready')