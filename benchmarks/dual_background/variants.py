# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import shutil,subprocess
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/dual-background-20261125';T=R/'benchmarks/dual_background';H=R/'benchmarks/results/compaction-acceptance-20261125'
shutil.copytree(H/'engine-snapshot',O/'serial');shutil.copytree(O/'engine-snapshot',O/'current-serial')
p=O/'current-serial/lib/db.cc';s=p.read_text();old=(O/'serial/lib/db.cc').read_text();loop=old[old.index('void DB::BackgoundLoop()'):old.index('void DB::RotateMemTableLocked()')];loop=loop.replace('void DB::BackgoundLoop()','void DB::FlushLoop()').replace('backgroundError_ = std::current_exception();','if(!backgroundError_) backgroundError_ = std::current_exception();');a=s.index('void DB::FlushLoop()');b=s.index('void DB::RotateMemTableLocked()');s=s[:a]+loop+s[b:];s=s.replace('        compactionThread_ = std::thread([this]{ this->CompactionLoop(); });','        // Isolated control: FlushLoop schedules both task kinds.');p.write_text(s)
for name in ['serial','current-serial']:
 p=subprocess.run(['diff','-ruN',str(O/name),str(O/'engine-snapshot')],stdout=subprocess.PIPE);(O/(name+'-vs-current.patch')).write_bytes(p.stdout)
# Snapshot old tests to run applicable tests, skip historical scheduling-specific DB Compaction.
shutil.copytree(H/'tests-snapshot',O/'historical-tests');shutil.copytree(H/'test-support-snapshot',O/'historical-support')
s=(H/'root-CMakeLists.txt').read_text();s=s[s.index('add_library(lsm_engine STATIC'):].replace('${CMAKE_SOURCE_DIR}/src/lsm/include',str(O/'serial/include')).replace('src/lsm/lib/',str(O/'serial/lib')+'/').replace('tests/lsm/',str(O/'historical-tests')+'/').replace('${CMAKE_SOURCE_DIR}/tests/kvraft',str(O/'historical-support'))
(O/'historical-test-project').mkdir();(O/'historical-test-project/CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(t LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\nfind_package(Threads REQUIRED)\n'+s)
# reuse helper functions, not legacy main
s=(R/'benchmarks/compaction_acceptance/bench.cc').read_text();s=s[:s.index('int main(')];(T/'base.cc').write_text(s)
# Independent instrumentation based on previous fixed thread-local probe; no existing global TaskTrace.
shutil.copy(R/'benchmarks/compaction_diagnosis/dd.h',T/'dd.h')
for kind in ['serial','engine-snapshot']:
 E=O/(kind+'-probe');shutil.copytree(O/kind,E)
 p=E/'lib/db.cc';s=p.read_text();s='\n'.join(line.rstrip() for line in s.splitlines())+'\n'
 s=s.replace('void DB::Write(WriteBatch& batch)\n{','void DB::Write(WriteBatch& batch)\n{\n DD_SCOPE(WriteApi);')
 s=s.replace('    std::unique_lock<std::mutex> writeQueueLock(writeQueueMutex_);','    std::unique_lock<std::mutex> writeQueueLock(writeQueueMutex_,std::defer_lock); {DD_SCOPE(QueueLock);writeQueueLock.lock();}')
 s=s.replace('    pending.cv.wait(writeQueueLock, [&,this](){','    DD_TIME(QueueWait,pending.cv.wait(writeQueueLock, [&,this](){').replace('return pending.done || writeQueue_.front() == &pending;\n    });','return pending.done || writeQueue_.front() == &pending;\n    }));')
 s=s.replace('    writeQueueLock.unlock();','    DD_COUNT(GroupSize,group.size());\n    writeQueueLock.unlock();',1)
 a=s.index('void DB::WriteGroup');s=s[:a]+s[a:].replace('    std::unique_lock<std::mutex> lock(dbMutex_);','    std::unique_lock<std::mutex> lock(dbMutex_,std::defer_lock);{DD_SCOPE(FirstLock);lock.lock();}',1).replace('LSM_TIME(Backpressure, backgroundCv_.wait','DD_TIME(Backpressure, backgroundCv_.wait',1)
 s=s.replace('    LSM_SCOPE(Rotate);','    DD_SCOPE(Rotate); LSM_SCOPE(Rotate);')
 a=s.index('        // 4. 写入 MemTable',s.index('void DB::WriteGroup'));b=s.index('        for(auto p : group)',a);s=s[:b]+'        DD_SCOPE(Add);\n'+s[b:]
 s=s.replace('void DB::FlushImmutable(const MemTableState& state)\n{','void DB::FlushImmutable(const MemTableState& state)\n{\n DD_PHASE(5);DD_SCOPE(FlushTask);')
 s=s.replace('    LSM_LOCK_START(diag_build);','    LSM_LOCK_START(diag_build);auto dd_build_start=dd::now();',1).replace('    LSM_LOCK_END(diag_build, Build);','    LSM_LOCK_END(diag_build, Build);dd::record(dd::FlushBuild,dd_build_start,dd::now(),true,true);',1)
 s=s.replace('LSM_TIME(Writer, SSTableWriter::Write(sstablePath, builder))','DD_TIME(FlushWriter,SSTableWriter::Write(sstablePath,builder))',1)
 s=s.replace('        LSM_LOCK_START(diag_publish);\n        std::lock_guard<std::mutex> lock(dbMutex_);','        LSM_LOCK_START(diag_publish);\n        std::unique_lock<std::mutex> lock(dbMutex_,std::defer_lock);{DD_SCOPE(FlushPublishWait);lock.lock();}\n        DD_SCOPE(FlushPublishHold);',1)
 s=s.replace('LSM_TIME(Manifest, tmpManifest.Save(manifestPath_))','DD_TIME(FlushManifest,tmpManifest.Save(manifestPath_))',1)
 s=s.replace('void DB::CompactTables(uint64_t taskRequest)\n{','void DB::CompactTables(uint64_t taskRequest)\n{\n DD_PHASE(6);DD_SCOPE(CompactTask);')
 s=s.replace('std::vector<TableMeta> outputMetas = BuildCompactionOutputs(inputs);','std::vector<TableMeta> outputMetas = DD_TIME(MergeBuild,BuildCompactionOutputs(inputs));')
 s=s.replace('SSTableWriter::Write(TablePath(currentMeta->fileNumber), builder.value())','DD_TIME(OutputWrite,SSTableWriter::Write(TablePath(currentMeta->fileNumber),builder.value()))')
 a=s.index('void DB::CompactTables');pre=s[:a];b=s[a:];mark='    // 4. 锁内：发布';i=b.index(mark);b=b[:i]+b[i:].replace('        std::lock_guard<std::mutex> lock(dbMutex_);','        std::unique_lock<std::mutex> lock(dbMutex_,std::defer_lock);{DD_SCOPE(PublishWait);lock.lock();}\n        DD_SCOPE(PublishHold);',1);b=b.replace('tmpManifest.Save(manifestPath_);','DD_TIME(Manifest,tmpManifest.Save(manifestPath_));',1).replace('    inputs.clear();','    DD_SCOPE(Cleanup);\n    inputs.clear();',1);s=pre+b;p.write_text(s)
 (O/(kind+'-probe.patch')).write_bytes(subprocess.run(['diff','-ruN',str(O/kind),str(E)],stdout=subprocess.PIPE).stdout)
print('variants ready')
