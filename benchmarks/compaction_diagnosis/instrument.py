# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import shutil
R=Path('/home/truth/Code/my_kvraft');T=R/'benchmarks/compaction_diagnosis';O=R/'benchmarks/results/compaction-diagnosis-20261125';E=O/'instrumented'
shutil.copytree(O/'original',E,dirs_exist_ok=True)
for p in (E/'lib').glob('*.cc'):p.write_text('\n'.join(line.rstrip() for line in p.read_text().splitlines())+'\n')
def edit(file,a,b,count=1):
 p=E/file;s=p.read_text();assert a in s,(file,a);s=s.replace(a,b,count);p.write_text(s)
edit('lib/sstable.cc','    size_t blockIndex = FindStartBlock(target);','    DD_COUNT(IndexItems,index_.size());\n    size_t blockIndex; {DD_QUERY(Index); blockIndex = FindStartBlock(target);}')
edit('lib/sstable.cc','    LookupResult result;        // 默认为 kNotFound','    DD_COUNT(Tables,1);\n    LookupResult result;        // 默认为 kNotFound')
edit('lib/sstable.cc','    if(!bloomFilter_->MayContain(userKey))','    DD_COUNT(BloomChecks,1);\n    if(!bloomFilter_->MayContain(userKey))')
edit('lib/sstable.cc','{lsm_diag::count(lsm_diag::BloomRejects); return result;}','{DD_COUNT(BloomRejects,1); lsm_diag::count(lsm_diag::BloomRejects); return result;}')
edit('lib/sstable.cc','    if(blockCache_) reader = blockCache_->Get(cacheKey);','    if(blockCache_) {DD_QUERY(Cache); reader = blockCache_->Get(cacheKey); DD_COUNT(reader?Hits:Misses,1);}')
# DD_COUNT ternary macro cannot resolve names: replace manually
p=E/'lib/sstable.cc';s=p.read_text().replace('DD_COUNT(reader?Hits:Misses,1);','if(reader) DD_COUNT(Hits,1); else DD_COUNT(Misses,1);');p.write_text(s)
edit('lib/sstable.cc','        std::string rawBlock = ReadBlock(offset, size);','        DD_COUNT(Load,1); DD_COUNT(BlockBytes,size);\n        std::string rawBlock = ReadBlock(offset, size);')
edit('lib/sstable.cc','    return reader;','    if(blockCache_) DD_COUNT(Usage,blockCache_->Usage());\n    return reader;')
edit('lib/data_block.cc','    InternalKey target;','    DD_QUERY(BlockGet);\n    InternalKey target;')
edit('lib/data_block.cc','    lsm_diag::count(lsm_diag::ScannedRecords, diagnosticScanned);','    DD_COUNT(Scanned,diagnosticScanned);\n    lsm_diag::count(lsm_diag::ScannedRecords, diagnosticScanned);')
edit('lib/db.cc','void DB::Write(WriteBatch& batch)\n{','void DB::Write(WriteBatch& batch)\n{\n    DD_SCOPE(WriteApi);')
edit('lib/db.cc','    std::unique_lock<std::mutex> writeQueueLock(writeQueueMutex_);','    std::unique_lock<std::mutex> writeQueueLock(writeQueueMutex_,std::defer_lock); {DD_SCOPE(QueueLock); writeQueueLock.lock();}')
edit('lib/db.cc','    pending.cv.wait(writeQueueLock, [&,this](){','    DD_TIME(QueueWait, pending.cv.wait(writeQueueLock, [&,this](){')
edit('lib/db.cc','        return pending.done || writeQueue_.front() == &pending;\n    });','        return pending.done || writeQueue_.front() == &pending;\n    }));')
edit('lib/db.cc','    writeQueueLock.unlock();','    DD_COUNT(GroupSize,group.size());\n    writeQueueLock.unlock();')
edit('lib/db.cc','void DB::WriteGroup(const std::vector<PendingWrite*>& group)\n{','void DB::WriteGroup(const std::vector<PendingWrite*>& group)\n{')
# unique first DB lock in WriteGroup only
p=E/'lib/db.cc';s=p.read_text();idx=s.index('void DB::WriteGroup');a=s[:idx];b=s[idx:].replace('    std::unique_lock<std::mutex> lock(dbMutex_);','    std::unique_lock<std::mutex> lock(dbMutex_,std::defer_lock); {DD_SCOPE(FirstLock);lock.lock();}',1).replace('LSM_TIME(Backpressure, backgroundCv_.wait','DD_TIME(Backpressure, backgroundCv_.wait',1)
s=a+b;p.write_text(s)
edit('lib/db.cc','    LSM_SCOPE(Rotate);','    DD_SCOPE(Rotate); LSM_SCOPE(Rotate);')
edit('lib/db.cc','LSM_TIME(Add, active_.table->Add','DD_TIME(Add, active_.table->Add')
# Per-entry timers would be too heavy: replace Add timer by outer loop timer spanning all Adds.
p=E/'lib/db.cc';s=p.read_text().replace('DD_TIME(Add, active_.table->Add','LSM_TIME(Add, active_.table->Add');marker='        // 4. 写入 MemTable';idx=s.index(marker,s.index('void DB::WriteGroup'));pos=s.index('        for(auto p : group)',idx);s=s[:pos]+'        DD_SCOPE(Add);\n'+s[pos:];p.write_text(s)
edit('lib/db.cc','void DB::FlushImmutable(const MemTableState& state)\n{','void DB::FlushImmutable(const MemTableState& state)\n{\n    DD_PHASE(6); DD_SCOPE(FlushTask);')
edit('lib/db.cc','void DB::CompactTables(uint64_t taskRequest)\n{','void DB::CompactTables(uint64_t taskRequest)\n{\n    DD_PHASE(6); DD_SCOPE(CompactTask);')
edit('lib/db.cc','std::vector<TableMeta> outputMetas = BuildCompactionOutputs(inputs);','std::vector<TableMeta> outputMetas = DD_TIME(MergeBuild, BuildCompactionOutputs(inputs));')
edit('lib/db.cc','SSTableWriter::Write(TablePath(currentMeta->fileNumber), builder.value())','DD_TIME(OutputWrite,SSTableWriter::Write(TablePath(currentMeta->fileNumber), builder.value()))',2)
# Scope reader preparation and cleanup; publication lock timer starts before acquisition.
p=E/'lib/db.cc';s=p.read_text();idx=s.index('void DB::CompactTables');a=s[:idx];b=s[idx:];b=b.replace('    for(const auto& meta : outputMetas)','    {DD_SCOPE(CompactReaders);\n    for(const auto& meta : outputMetas)',1).replace('    // 4. 锁内：发布','    }\n    // 4. 锁内：发布',1);b=b.replace('        std::lock_guard<std::mutex> lock(dbMutex_);\n        // 构造候选','        std::unique_lock<std::mutex> lock(dbMutex_,std::defer_lock); {DD_SCOPE(PublishWait);lock.lock();}\n        DD_SCOPE(PublishHold);\n        // 构造候选',1).replace('        tmpManifest.Save(manifestPath_);','        DD_TIME(Manifest,tmpManifest.Save(manifestPath_));',1).replace('    inputs.clear();','    DD_SCOPE(Cleanup);\n    inputs.clear();',1);s=a+b
# flush existing diagnostic markers allow independent timers
s=s.replace('    LSM_LOCK_END(diag_build, Build);','    LSM_LOCK_END(diag_build, Build);\n#ifdef PROBE\n    dd::record(dd::FlushBuild, dd_flush_build_start,dd::now(),true,true);\n#endif',1).replace('    LSM_LOCK_START(diag_build);','    LSM_LOCK_START(diag_build);\n#ifdef PROBE\n    auto dd_flush_build_start=dd::now();\n#endif',1).replace('LSM_TIME(Writer, SSTableWriter::Write(sstablePath, builder))','DD_TIME(FlushWriter, SSTableWriter::Write(sstablePath, builder))',1).replace('LSM_TIME(Manifest, tmpManifest.Save(manifestPath_))','DD_TIME(FlushManifest, tmpManifest.Save(manifestPath_))',1)
s=s.replace('        LSM_LOCK_START(diag_publish);\n        std::lock_guard<std::mutex> lock(dbMutex_);','        LSM_LOCK_START(diag_publish);\n        std::unique_lock<std::mutex> lock(dbMutex_,std::defer_lock); {DD_SCOPE(FlushPublishWait);lock.lock();}\n        DD_SCOPE(FlushPublishHold);',1)
p.write_text(s)
# Own base harness, original immutable
s=(O/'original-bench.cc').read_text();s=s.replace('int main(int argc,char** argv)','int legacy_main(int argc,char** argv)');s=s.replace('auto a=Clock::now();if(writing)db->Write(batches[i]);','auto a=Clock::now();if(writing){DD_PHASE(3);db->Write(batches[i]);}');s=s.replace('auto f=Clock::now();db->Flush();','DD_PHASE(4);auto f=Clock::now();db->Flush();');(T/'base.cc').write_text(s)
print('isolated instrumentation ready')
