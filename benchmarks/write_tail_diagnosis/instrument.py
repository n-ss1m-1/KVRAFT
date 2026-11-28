# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/write_tail_diagnosis';O=R/'benchmarks/results/write-tail-diagnosis-20261125'
p=T/'dd.h';s=p.read_text();extra=['Ready','Claim','ImmutableRemoved','ReaderPrepare','ManifestEncode','ManifestWrite','FileSync','DirSync','Rename','FileWrite','Unlink','DbHold','FileId','InputId','QueueRelock','TaskLockWait']
s=s.replace('FlushManifest,Count','FlushManifest,'+','.join(extra)+',Count').replace('"flush_manifest"};','"flush_manifest",'+','.join('"'+x+'"' for x in extra)+'};')
s=s.replace('int metric,phase;','int metric,phase;uint64_t task,file,request;')
s=s.replace('events[8192]','events[32768]').replace('s->used<8192','s->used<32768')
s=s.replace('inline Slot slots[8];','inline thread_local uint64_t task=0,file=0,request=0;\ninline Slot slots[8];')
s=s.replace('{a,b,m,phase}','{a,b,m,phase,task,file,request}')
s=s.replace('struct Phase{','inline void mark(M m)noexcept{auto n=now();record(m,n,n,true,true);}\nstruct Phase{')
s=s.replace('<<e.b<<\'}\';','<<e.b<<",\\\"task\\\":"<<e.task<<",\\\"file\\\":"<<e.file<<",\\\"request\\\":"<<e.request<<\'}\';')
s=s.replace('f<<"{\\\"slots\\\":','f<<"{\\\"event_capacity_per_slot\\\":32768,\\\"slots\\\":')
# errno preserved even if timer/destructor clock implementation changes it
s=s.replace('#include <cstdlib>','#include <cstdlib>\n#include <cerrno>\n#include <unistd.h>\n#include <cstdio>')
pos=s.index('\n}\n#define DD_SCOPE')
s=s[:pos]+'''
inline int sync(int fd,bool dir) noexcept{auto a=now();int r=::fsync(fd),e=errno;record(dir?DirSync:FileSync,a,now(),true,true);errno=e;return r;}
inline int renameFile(const char*a,const char*b)noexcept{auto n=now();int r=::rename(a,b),e=errno;record(Rename,n,now(),true,true);errno=e;return r;}
inline int removeFile(const char*a)noexcept{auto n=now();int r=::unlink(a),e=errno;record(Unlink,n,now(),true,true);errno=e;return r;}
'''+s[pos:];p.write_text(s)
# benchmark absolute epoch for alignment; request ordinal set before each API. verification kept separate phase.
p=T/'bench.cc';s=p.read_text().replace('auto a=Clock::now();if(writing)','auto a=Clock::now();\n#ifdef PROBE\ndd::request=i;\n#endif\nif(writing)').replace('std::cout<<"{\\\"mode', 'std::cout<<"{\\\"epoch_ns\\\":"<<std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count()<<",\\\"mode');p.write_text(s)
p=O/'probe/lib/db.cc';s=p.read_text()
s=s.replace('immutable_ = std::move(active_);','immutable_ = std::move(active_);\n {auto old=dd::task;dd::task=reinterpret_cast<uintptr_t>(immutable_->table.get());dd::mark(dd::Ready);dd::task=old;}')
s=s.replace('if(immutable_.has_value()) task = immutable_.value();','if(immutable_.has_value()) task = immutable_.value();\n dd::task=reinterpret_cast<uintptr_t>(task.table.get());dd::mark(dd::Claim);')
s=s.replace('if(compactionRequested_ > compactionCompleted_) taskRequest = compactionRequested_;','if(compactionRequested_ > compactionCompleted_) taskRequest = compactionRequested_;\n dd::task=taskRequest;dd::mark(dd::Claim);')
s=s.replace('++compactionRequested_;','++compactionRequested_;\n {auto old=dd::task;dd::task=compactionRequested_;dd::mark(dd::Ready);dd::task=old;}')
s=s.replace('fileNumber = nextFileNumber_++;','fileNumber = nextFileNumber_++;dd::file=fileNumber;dd::mark(dd::FileId);')
s=s.replace('currentMeta->fileNumber = nextFileNumber_++;','currentMeta->fileNumber = nextFileNumber_++;dd::file=currentMeta->fileNumber;dd::mark(dd::FileId);')
s=s.replace('dd::file=currentMeta->fileNumber;dd::mark(dd::FileId);dd::file=fileNumber;dd::mark(dd::FileId);','dd::file=currentMeta->fileNumber;dd::mark(dd::FileId);')
s=s.replace('inputMetas = manifest_.Tables();','inputMetas = manifest_.Tables();\n for(auto& m:inputMetas){dd::file=m.fileNumber;dd::mark(dd::InputId);}dd::file=0;')
s=s.replace('immutable_.reset();','immutable_.reset();dd::mark(dd::ImmutableRemoved);')
s=s.replace('LSM_TIME(ReaderInit, std::make_shared<SSTableReader>(sstablePath, fileNumber, blockCache_))','DD_TIME(ReaderPrepare,std::make_shared<SSTableReader>(sstablePath, fileNumber, blockCache_))')
s=s.replace('    writeQueueLock.lock();','    {DD_SCOPE(QueueRelock);writeQueueLock.lock();}')
# first lock hold split at CV wait. explicit release timer before CV and restart after reacquisition
start=s.index('void DB::WriteGroup');end=s.index('std::vector<TableMeta> DB::BuildCompactionOutputs',start);part=s[start:end]
part=part.replace('    CheckHealthLocked();','    auto dd_hold=dd::now();\n    struct EndHold{uint64_t& a;~EndHold(){dd::record(dd::DbHold,a,dd::now(),true,true);}} dd_end{dd_hold};\n    CheckHealthLocked();',1)
part=part.replace('            DD_TIME(Backpressure,','            dd::record(dd::DbHold,dd_hold,dd::now(),true,true);\n            DD_TIME(Backpressure,').replace('            CheckHealthLocked();','            dd_hold=dd::now();\n            CheckHealthLocked();')
s=s[:start]+part+s[end:]
# task acquisition lock wait, but CV lifetime not classified as lock held
s=s.replace('std::unique_lock<std::mutex> lock(dbMutex_);        // unique_lock','std::unique_lock<std::mutex> lock(dbMutex_,std::defer_lock);{DD_SCOPE(TaskLockWait);lock.lock();} // unique_lock')
s=s.replace('fsync(dirFd)','dd::sync(dirFd,true)').replace('fsync(fd)','dd::sync(fd,false)').replace('unlink(', 'dd::removeFile(')
p.write_text(s)
for n in ['sstable','manifest']:
 p=O/f'probe/lib/{n}.cc';s=p.read_text();s=s.replace('fsync(dirFd)','dd::sync(dirFd,true)').replace('fsync(tmpFd)','dd::sync(tmpFd,false)').replace('fsync(fd)','dd::sync(fd,false)').replace('rename(', 'dd::renameFile(').replace('unlink(', 'dd::removeFile(')
 if n=='manifest':s=s.replace('EncodeManifest(*this)','DD_TIME(ManifestEncode,EncodeManifest(*this))').replace('WriteAll(tmpFd, encoded.data(), encoded.size());','DD_TIME(ManifestWrite,WriteAll(tmpFd,encoded.data(),encoded.size()));')
 else:
  # one timer over serialization/writes before file sync (includes encoding and CRC)
  s=s.replace('        // 3. 写入 Data Blocks','        auto dd_writes=dd::now();\n        // 3. 写入 Data Blocks').replace('        int syncResult;','        dd::record(dd::FileWrite,dd_writes,dd::now(),true,true);\n        int syncResult;')
 p.write_text(s)
(O/'probe.patch').write_bytes(subprocess.run(['diff','-ruN',str(O/'engine-snapshot'),str(O/'probe')],stdout=subprocess.PIPE).stdout)
s='''cmake_minimum_required(VERSION 3.16)
project(write_tail LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
option(LSM_DIAGNOSTICS "must be off" OFF)
if(LSM_DIAGNOSTICS)
message(FATAL_ERROR "must be OFF")
endif()
foreach(kind engine-snapshot probe)
file(GLOB sources ${RESULT_ROOT}/${kind}/lib/*.cc)
add_library(e_${kind} STATIC ${sources})
target_include_directories(e_${kind} PUBLIC ${RESULT_ROOT}/${kind}/include PRIVATE ${RESULT_ROOT}/${kind}/include/lsm ${CMAKE_CURRENT_SOURCE_DIR})
if(kind STREQUAL "probe")
target_compile_definitions(e_${kind} PUBLIC PROBE=1)
target_compile_options(e_${kind} PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/dd.h)
endif()
add_executable(bench_${kind} bench.cc)
target_link_libraries(bench_${kind} PRIVATE e_${kind} Threads::Threads)
endforeach()
''';(T/'CMakeLists.txt').write_text(s)
print('instrumented')