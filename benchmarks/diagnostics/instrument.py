from pathlib import Path
r=Path('/home/truth/Code/my_kvraft')
p=r/'src/lsm/lib/db.cc';s=p.read_text();s=s.replace('#include "db.h"','#include "db.h"\n#include "diagnostics.h"')
# Public operation context is supplied by harness; engine children inherit it.
for signature in ['std::optional<std::string> DB::Get(std::string_view key) const','void DB::Flush()','void DB::Write(lsm::Entry entry)']:
 start=s.index(signature); brace=s.index('{',start);lock=s.index('lock(dbMutex_);',brace)+len('lock(dbMutex_);')
 segment=s[brace:lock];segment=segment.replace('    std::','    LSM_LOCK_START(diag_first);\n    std::',1)+'\n    LSM_LOCK_END(diag_first, FirstLock);'
 s=s[:brace]+segment+s[lock:]
s=s.replace('memTable_', 'memTable_')
s=s.replace('active_.table->Get(publishedSequence_, std::string(key))','LSM_TIME(ActiveGet, active_.table->Get(publishedSequence_, std::string(key)))').replace('immutable_->table->Get(publishedSequence_, std::string(key))','LSM_TIME(ImmutableGet, immutable_->table->Get(publishedSequence_, std::string(key)))')
s=s.replace('result = reader->Get(publishedSequence_, key);','lsm_diag::count(lsm_diag::Tables);\n        result = reader->Get(publishedSequence_, key);')
# Wrap predicate wait without changing lock or predicate semantics.
start=0
while True:
 a=s.find('backgroundCv_.wait(lock, [&](){',start)
 if a<0:break
 b=s.index('});',a)+3
 metric='FlushWait' if a<s.index('void DB::Write') else 'Backpressure'
 if a>s.index('void DB::BackgoundLoop'):break
 expr=s[a:b-1];replacement='LSM_TIME('+metric+', '+expr+');'
 s=s[:a]+replacement+s[b:];start=a+len(replacement)
s=s.replace('std::error_code ec = walWriter_->Append(entry);','std::error_code ec = LSM_TIME(Append, walWriter_->Append(entry));').replace('ec = walWriter_->Sync();','ec = LSM_TIME(Sync, walWriter_->Sync());')
s=s.replace('active_.table->Add(entry.key.sequence, entry.key.type, entry.key.user_key, entry.value)','LSM_TIME(Add, active_.table->Add(entry.key.sequence, entry.key.type, entry.key.user_key, entry.value))')
s=s.replace('void DB::RotateMemTableLocked() \n{','void DB::RotateMemTableLocked() \n{\n    LSM_SCOPE(Rotate);')
s=s.replace('immutable_ = std::move(active_);','immutable_ = std::move(active_);\n#ifdef LSM_DIAGNOSTICS\n    immutable_->diagnosticOrigin = lsm_diag::phase;\n#endif')
s=s.replace('void DB::FlushImmutable(const MemTableState& state) \n{','void DB::FlushImmutable(const MemTableState& state) \n{\n#ifdef LSM_DIAGNOSTICS\n    lsm_diag::Context taskContext(static_cast<lsm_diag::Phase>(state.diagnosticOrigin), lsm_diag::Background);\n#endif')
s=s.replace('    SSTableBuilder builder(state.table->Size());','    LSM_LOCK_START(diag_build);\n    SSTableBuilder builder(state.table->Size());').replace('    builder.Finish();','    builder.Finish();\n    LSM_LOCK_END(diag_build, Build);')
s=s.replace('SSTableWriter::Write(sstablePath, builder)','LSM_TIME(Writer, SSTableWriter::Write(sstablePath, builder))').replace('std::make_unique<SSTableReader>(sstablePath)','LSM_TIME(ReaderInit, std::make_unique<SSTableReader>(sstablePath))')
s=s.replace('        std::lock_guard<std::mutex> lock(dbMutex_);\n        tables_.reserve','        LSM_LOCK_START(diag_publish);\n        std::lock_guard<std::mutex> lock(dbMutex_);\n        LSM_LOCK_END(diag_publish, PublishWait);\n        LSM_SCOPE(PublishHold);\n        tables_.reserve')
s=s.replace('tmpManifest.Save(manifestPath_);','LSM_TIME(Manifest, tmpManifest.Save(manifestPath_));').replace('DeleteObsoleteWals(state.walNumbers);','LSM_TIME(Cleanup, DeleteObsoleteWals(state.walNumbers));')
s=s.replace('if(backgoundThread_.joinable()) backgoundThread_.join();','if(backgoundThread_.joinable()) LSM_TIME(Join, backgoundThread_.join());')
s=s.replace('std::unique_ptr<WalWriter> DB::CreateWal(uint64_t walNumber) \n{','std::unique_ptr<WalWriter> DB::CreateWal(uint64_t walNumber) \n{\n    LSM_SCOPE(CreateWal);').replace('const std::error_code ec = writer->Sync();','const std::error_code ec = LSM_TIME(NewWalSync, writer->Sync());')
# Only CreateWal directory sync is a child of new-WAL creation.
a=s.index('std::unique_ptr<WalWriter> DB::CreateWal');b=s.index('void DB::DeleteObsoleteWals',a);part=s[a:b].replace('SyncDirectory();','LSM_TIME(DirectorySync, SyncDirectory());');s=s[:a]+part+s[b:]
s=s.replace('        std::lock_guard<std::mutex> lock(dbMutex_);\n\n        // 分配fileNumber','        LSM_LOCK_START(diag_number);\n        std::lock_guard<std::mutex> lock(dbMutex_);\n        LSM_LOCK_END(diag_number, FileNumberWait);\n        LSM_SCOPE(FileNumberHold);\n\n        // 分配fileNumber')
s=s.replace('                std::unique_lock<std::mutex> lock(dbMutex_);','                LSM_LOCK_START(diag_task);\n                std::unique_lock<std::mutex> lock(dbMutex_);').replace('                backgroundCv_.wait(lock, [&](){','                LSM_LOCK_END(diag_task, TaskLockWait);\n                backgroundCv_.wait(lock, [&](){')
p.write_text(s)
p=r/'src/lsm/include/lsm/db.h';s=p.read_text().replace('        std::shared_ptr<MemTable> table;','\n#ifdef LSM_DIAGNOSTICS\n        int diagnosticOrigin = 0;\n#endif\n        std::shared_ptr<MemTable> table;');p.write_text(s)
p=r/'src/lsm/lib/sstable.cc';s=p.read_text().replace('#include "sstable.h"','#include "sstable.h"\n#include "diagnostics.h"')
s=s.replace('if(!bloomFilter_->MayContain(userKey)) return result;','lsm_diag::count(lsm_diag::BloomChecks);\n    if(!bloomFilter_->MayContain(userKey)) {lsm_diag::count(lsm_diag::BloomRejects); return result;}').replace('size_t blockIndex = FindStartBlock(target);','lsm_diag::count(lsm_diag::IndexLookups);\n    size_t blockIndex = FindStartBlock(target);')
s=s.replace('DataBlockReader blockReader(ReadBlock(idx.offset, idx.size));','lsm_diag::count(lsm_diag::Blocks);\n        lsm_diag::count(lsm_diag::Bytes, idx.size + 4);\n        auto rawBlock = ReadBlock(idx.offset, idx.size);\n        DataBlockReader blockReader = LSM_TIME(Validate, DataBlockReader(std::move(rawBlock)));').replace('result = blockReader.Get(readSequence, userKey);','result = LSM_TIME(BlockGet, blockReader.Get(readSequence, userKey));')
s=s.replace('std::string SSTableReader::ReadBlock(uint64_t offset, uint64_t size) const \n{','std::string SSTableReader::ReadBlock(uint64_t offset, uint64_t size) const \n{\n    LSM_SCOPE(ReadBlock);')
s=s.replace('PreadAll(fd_, content.data(), size, offset);','LSM_TIME(Pread, PreadAll(fd_, content.data(), size, offset));').replace('PreadAll(fd_, rawCrc, 4, offset+size);','LSM_TIME(Pread, PreadAll(fd_, rawCrc, 4, offset+size));')
s=s.replace('lsm::Crc32c(content) != lsm::DecodeFixed32(rawCrc)','LSM_TIME(Crc, lsm::Crc32c(content) != lsm::DecodeFixed32(rawCrc))')
s=s.replace('            ssize_t n = pread(fd, output+done, size-done, offset+done);','            lsm_diag::count(lsm_diag::PreadCalls);\n            ssize_t n = pread(fd, output+done, size-done, offset+done);\n            if(n>0) lsm_diag::count(lsm_diag::PreadBytes, static_cast<uint64_t>(n));')
p.write_text(s)
p=r/'src/lsm/lib/data_block.cc';s=p.read_text().replace('#include "data_block.h"','#include "data_block.h"\n#include "diagnostics.h"')
s=s.replace('    while(offset < data_.size())','    uint64_t diagnosticRecords = 0;\n    while(offset < data_.size())').replace('        lsm::Entry current;','        ++diagnosticRecords;\n        lsm::Entry current;').replace('        offset = nextOffset;\n    }','        offset = nextOffset;\n    }\n    lsm_diag::count(lsm_diag::ValidatedRecords, diagnosticRecords);')
s=s.replace('    Iterator it = this->Begin();','    uint64_t diagnosticScanned = 0;\n    Iterator it = this->Begin();').replace('        if(!cmp(it.Key(),target)) break;','        ++diagnosticScanned;\n        if(!cmp(it.Key(),target)) break;').replace('    LookupResult result;','    lsm_diag::count(lsm_diag::ScannedRecords, diagnosticScanned);\n    LookupResult result;');p.write_text(s)
p=r/'CMakeLists.txt';s=p.read_text();s+='\n# Optional cost diagnostics; disabled in ordinary builds.\noption(LSM_DIAGNOSTICS "Enable LSM cost diagnostics" OFF)\nif(LSM_DIAGNOSTICS)\n target_compile_definitions(lsm_engine PUBLIC LSM_DIAGNOSTICS=1)\nendif()\n';p.write_text(s)
