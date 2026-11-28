# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

exec((__import__('pathlib').Path(__file__).parent/'paths.py').read_text())
p=O/'mechanism/lib/db.cc';s=p.read_text();s='#include "rw.h"\n'+s
s=s.replace('void DB::CompactTables(uint64_t taskRequest, uint32_t sourceLevel)\n{','void DB::CompactTables(uint64_t taskRequest, uint32_t sourceLevel)\n{\n rw::Task task(taskRequest,sourceLevel,options_.maxCompactionSourceBytes);')
a=s.index('void DB::CompactTables(');b=s.index('std::optional<uint32_t> DB::PickAuto',a);part=s[a:b]
part=part.replace('        if(levels_[sourceLevel].empty())','        rw::levels(levels_,task.beforeN,task.beforeB);\n        if(levels_[sourceLevel].empty())')
part=part.replace('    // 2. 锁外：BuildCompactionOutputs()', '''    task.selected=rw::now();
    for(auto&x:sourceInputs)task.source.push_back(rw::file(x.meta));
    for(auto&x:targetInputs)task.target.push_back(rw::file(x.meta));
    for(auto&x:task.source)rw::scan(x,TablePath(x.m.fileNumber));
    for(auto&x:task.target)rw::scan(x,TablePath(x.m.fileNumber));
    task.buildStart=rw::now();
    // 2. 锁外：BuildCompactionOutputs()''')
part=part.replace('    // 3. 锁外：准备新的SSTableState','''    task.buildEnd=rw::now();
    for(auto&m:outputMetas){auto f=rw::file(m);rw::scan(f,TablePath(m.fileNumber));task.output.push_back(f);}
    // 3. 锁外：准备新的SSTableState''')
part=part.replace('    std::vector<LevelTables> candidateLevels;','    std::vector<LevelTables> candidateLevels;\n    task.wait=rw::now();').replace('        CheckFailureLocked();','        task.locked=rw::now();\n        CheckFailureLocked();')
part=part.replace('        levels_.swap(candidateLevels);','        levels_.swap(candidateLevels);\n        task.published=rw::now();rw::levels(levels_,task.afterN,task.afterB);')
part=part.replace('    // 5. 锁外：释放旧的SSTableReader','    task.unlock=rw::now();\n    // 5. 锁外：释放旧的SSTableReader')
s=s[:a]+part+s[b:]
a=s.index('std::vector<TableMeta> DB::BuildCompactionOutputs');b=s.index('void DB::CompactTables',a);part=s[a:b].replace('            currentMeta->fileSize = SSTableWriter::Write','            rw::WriteScope rw_write;\n            currentMeta->fileSize = SSTableWriter::Write').replace('        currentMeta->fileSize = SSTableWriter::Write','        rw::WriteScope rw_tail;\n        currentMeta->fileSize = SSTableWriter::Write')
# scope ends after builder reset/push, hence writer interval includes tiny bookkeeping; avoid duplicate nested scope.
part=part.replace('            rw::WriteScope rw_write;\n    ','    ')
s=s[:a]+part+s[b:]
a=s.index('void DB::FlushImmutable');b=s.index('void DB::Write(',a);part=s[a:b]
part=part.replace('    uint64_t fileNumber;','    rw::Event event;event.id=rw::flushes.size()+1;event.start=rw::now();\n    uint64_t fileNumber;')
part=part.replace('    // 2. 锁内发布','    auto f=rw::file(newSSTable.meta);f.records=state.table->Size();for(auto it=state.table->Begin();it.Valid();it.Next())f.tombs+=it.Key().type==ValueType::kDeletion;event.output.push_back(f);\n    // 2. 锁内发布')
part=part.replace('        immutable_.reset();','        event.published=rw::now();\n        immutable_.reset();')
# append at end of function preceding DB::Write
pos=part.rfind('}');part=part[:pos]+'    event.end=rw::now();rw::flushes.push_back(std::move(event));\n'+part[pos:]
s=s[:a]+part+s[b:];p.write_text(s)
p=T/'bench.cc';s=p.read_text();s='#ifdef RW_MECHANISM\n#include "rw.h"\n#endif\n'+s;s=s.replace('fullcheck(d,oracle);saveOracle', '#ifdef RW_MECHANISM\nrw::dump(out+"-events.jsonl");\n#endif\nfullcheck(d,oracle);saveOracle');p.write_text(s)
(T/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(rewrite LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
option(LSM_DIAGNOSTICS "must be OFF" OFF)
if(LSM_DIAGNOSTICS)
message(FATAL_ERROR "diagnostics must be off")
endif()
foreach(kind observer mechanism)
file(GLOB sources ${RESULT_ROOT}/${kind}/lib/*.cc)
add_executable(bench_${kind} bench.cc ${sources})
target_include_directories(bench_${kind} PRIVATE ${RESULT_ROOT}/${kind}/include ${RESULT_ROOT}/${kind}/include/lsm ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bench_${kind} PRIVATE OBSERVE=1)
if(kind STREQUAL "mechanism")
target_compile_definitions(bench_${kind} PRIVATE RW_MECHANISM=1)
endif()
target_link_libraries(bench_${kind} PRIVATE Threads::Threads)
endforeach()
''')
cmd(['cmake','-S',str(T),'-B',str(O/'build'),'-DCMAKE_BUILD_TYPE=Release','-DLSM_DIAGNOSTICS=OFF','-DRESULT_ROOT='+str(O)],'configure.txt');cmd(['cmake','--build',str(O/'build'),'-j4'],'build.txt')
for kind in ['observer','mechanism']:
 diff=''
 for p in sorted((O/kind).rglob('*')):
  if p.is_file():
   rel=p.relative_to(O/kind);q=O/'original'/rel
   diff+=''.join(difflib.unified_diff(q.read_text().splitlines(True),p.read_text().splitlines(True),fromfile='original/'+str(rel),tofile=kind+'/'+str(rel)))
 (O/(kind+'.patch')).write_text(diff)
print('BUILD READY',flush=True)