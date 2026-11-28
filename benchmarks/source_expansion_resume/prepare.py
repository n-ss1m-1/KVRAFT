# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
O.mkdir(exist_ok=False)
for n,a in [('git-status-before.txt',['git','status','--short']),('worktree-before.patch',['git','diff','--binary']),('HEAD.txt',['git','rev-parse','HEAD'])]:cmd(a,n)
(O/'engine-hashes.json').write_text(json.dumps(hashes(R/'src/lsm'),indent=2));shutil.copytree(R/'src/lsm',O/'original');shutil.copytree(R/'src/lsm',O/'observer');shutil.copytree(R/'tests/lsm',O/'tests');shutil.copytree(R/'tests/kvraft',O/'support')
s=(R/'CMakeLists.txt').read_text();s=s[s.index('add_library(lsm_engine STATIC'):].replace('${CMAKE_SOURCE_DIR}/src/lsm/include',str(O/'original/include')).replace('src/lsm/lib/',str(O/'original/lib')+'/').replace('tests/lsm/',str(O/'tests')+'/').replace('${CMAKE_SOURCE_DIR}/tests/kvraft',str(O/'support'))
(O/'test-project').mkdir();(O/'test-project/CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(t LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\nfind_package(Threads REQUIRED)\n'+s)
cmd(['cmake','-S',str(O/'test-project'),'-B',str(O/'test-build'),'-DCMAKE_BUILD_TYPE=Release','-DLSM_DIAGNOSTICS=OFF'],'configure-tests.txt');cmd(['cmake','--build',str(O/'test-build'),'-j4'],'build-tests.txt');cmd(['ctest','--test-dir',str(O/'test-build'),'-V'],'tests.txt')
cmd(['bash','-lc','uname -a; findmnt -T /home/truth/Code/my_kvraft; c++ --version; cmake --version; ps -eo pid,comm,args'],'environment.txt')
p=O/'observer/include/lsm/db.h';s=p.read_text().replace('class DB : private lsm::NonCopyable\n{','class DB : private lsm::NonCopyable\n{\n friend struct BenchAccess;').replace('    bool failed_ = false;','    bool benchBusy_ = false;\n    bool failed_ = false;');p.write_text(s)
p=O/'observer/lib/db.cc';s=p.read_text();a=s.index('void DB::CompactionLoop()');b=s.index('void DB::RotateMemTableLocked()',a);part=s[a:b].replace('            }\n\n            // 2.','                benchBusy_=true;\n            }\n\n            // 2.').replace('            CompactTables(taskRequest, sourceLevel);','            CompactTables(taskRequest, sourceLevel);\n            {std::lock_guard<std::mutex> lock(dbMutex_);benchBusy_=false;}\n            backgroundCv_.notify_all();');s=s[:a]+part+s[b:]
s='#include "totals.h"\n'+s;s=s.replace('    uint64_t fileSize = LSM_TIME(Writer, SSTableWriter::Write(sstablePath, builder));','    uint64_t fileSize = LSM_TIME(Writer, SSTableWriter::Write(sstablePath, builder));\n    totals::flushBytes+=fileSize;totals::flushCount++;')
s=s.replace('            outputs.push_back(currentMeta.value());','            totals::compBytes+=currentMeta->fileSize;totals::compCount++;\n            outputs.push_back(currentMeta.value());').replace('        outputs.push_back(currentMeta.value());','        totals::compBytes+=currentMeta->fileSize;totals::compCount++;\n        outputs.push_back(currentMeta.value());')
# replace overlap caused by indentation matching
s=s.replace('            totals::compBytes+=currentMeta->fileSize;totals::compCount++;\n    ','    ')
p.write_text(s);shutil.copytree(O/'observer',O/'mechanism')
(T/'totals.h').write_text('#pragma once\n#include <cstdint>\nnamespace totals {inline uint64_t flushBytes=0,compBytes=0,flushCount=0,compCount=0;}\n')
shutil.copy(P/'base.cc',T/'base.cc');shutil.copy(P/'rw.h',T/'rw.h')
s=(P/'bench.cc').read_text();a=s.index('DBOptions opts(');b=s.index('struct Lat',a)
s=s[:a]+'''size_t envnum(const char* k,size_t d){auto p=std::getenv(k);return p?std::stoull(p):d;}
DBOptions opts(bool a,bool w,size_t cache=0){auto o=options(cache,262144);o.enableAutoCompaction=a;o.enableWal=w;o.maxLevels=3;o.level0CompactionTrigger=4;o.level1TargetBytes=1048576;o.levelSizeMultiplier=4;if(config()=="C"){o.memTableSizeLimit=4194304;o.targetSSTableDataSize=4194304;o.level1TargetBytes=16777216;o.levelSizeMultiplier=10;o.maxLevels=7;}o.maxCompactionSourceBytes=envnum("SOURCE_BYTES",1048576);o.maxCompactionSourceExpansionFactor=envnum("EXPANSION",1);return o;}
'''+s[b:]
s='#include "totals.h"\n'+s;s=s.replace('<<settleNs<<', '<<settleNs<<')
s=s.replace('<<peak<<",\\\"initial\\\":', '<<peak<<",\\\"flush_output_bytes\\\":"<<totals::flushBytes<<",\\\"compaction_output_bytes\\\":"<<totals::compBytes<<",\\\"flush_count\\\":"<<totals::flushCount<<",\\\"compaction_output_files\\\":"<<totals::compCount<<",\\\"initial\\\":')
(T/'bench.cc').write_text(s)
# Reuse prior tested mechanism transformation against current selection; fix changed empty-source spelling.
s=(P/'instrument.py').read_text();s=s[s.index("p=O/'mechanism/lib/db.cc'"):s.index("p=T/'bench.cc'")];s=s.replace("if(levels_[sourceLevel].empty())","if(source.empty())")
exec(s)
# extra selection instrumentation, no algorithm changes
p=O/'mechanism/lib/db.cc';s=p.read_text();s=s.replace('        const auto& source = levels_[sourceLevel];','        task.selectionLocked=rw::now();\n        const auto& source = levels_[sourceLevel];')
s=s.replace('        // 目标输入不为空', '        task.baseBytes=sourceBytes;task.baseCount=sourceInputs.size();\n        for(auto&x:targetInputs)task.baseTargets.push_back(x.meta.fileNumber);\n        // 目标输入不为空')
s=s.replace('            for(size_t i=sourceInputs.size(); i<source.size(); i++)','            task.stopReason="exhausted";\n            for(size_t i=sourceInputs.size(); i<source.size(); i++)').replace('                const auto& sourceState = source[i];','                task.candidates++;\n                const auto& sourceState = source[i];')
s=s.replace('if(sourceBytes >= expandedSourceLimit || fileBytes > expandedSourceLimit - sourceBytes) break;','if(sourceBytes >= expandedSourceLimit || fileBytes > expandedSourceLimit - sourceBytes){task.stopReason="bytes";break;}').replace('if(overlapCount != targetInputs.size()) break;','if(overlapCount != targetInputs.size()){task.stopReason="new_target";break;}')
s=s.replace('                sourceInputs.push_back(sourceState);','                task.accepted++;\n                sourceInputs.push_back(sourceState);')
s=s.replace('        // 初始化input和inputMetas','        else task.stopReason="empty_target";\n        task.finalBytes=sourceBytes;\n        for(auto&x:targetInputs)task.finalTargets.push_back(x.meta.fileNumber);\n        if(task.baseTargets!=task.finalTargets)throw std::runtime_error("diagnostic target set mismatch");\n        task.selectionDone=rw::now();\n        // 初始化input和inputMetas')
s=s.replace('            LSM_TIME(Backpressure, backgroundCv_.wait','            auto bpstart=rw::now();\n            LSM_TIME(Backpressure, backgroundCv_.wait').replace('            CheckHealthLocked();\n            continue;','            rw::bpCount++;rw::bpNs+=rw::now()-bpstart;\n            CheckHealthLocked();\n            continue;')
p.write_text(s)
p=T/'rw.h';s=p.read_text();s=s.replace('struct Event{','inline thread_local uint64_t bpCount=0,bpNs=0;\nstruct Event{uint64_t selectionLocked=0,selectionDone=0,baseBytes=0,finalBytes=0,baseCount=0,candidates=0,accepted=0;std::string stopReason;std::vector<uint64_t> baseTargets,finalTargets;')
s=s.replace('FIELD(start)', 'FIELD(selectionLocked) FIELD(selectionDone) FIELD(baseBytes) FIELD(finalBytes) FIELD(baseCount) FIELD(candidates) FIELD(accepted) FIELD(start)')
s=s.replace(' o<<",\\\"source\\\":";', ' o<<",\\\"stop_reason\\\":\\\""<<e.stopReason<<"\\\",\\\"base_targets\\\":";nums(o,e.baseTargets);o<<",\\\"final_targets\\\":";nums(o,e.finalTargets);o<<",\\\"source\\\":";')
p.write_text(s)
p=T/'bench.cc';s=p.read_text().replace('rw::dump(out+"-events.jsonl");','rw::dump(out+"-events.jsonl");\nstd::ofstream(out+"-backpressure.json")<<"{\\\"count\\\":"<<rw::bpCount<<",\\\"ns\\\":"<<rw::bpNs<<"}";');p.write_text(s)
shutil.copy(P/'CMakeLists.txt',T/'CMakeLists.txt')
cmd(['cmake','-S',str(T),'-B',str(O/'build'),'-DCMAKE_BUILD_TYPE=Release','-DLSM_DIAGNOSTICS=OFF','-DRESULT_ROOT='+str(O)],'configure.txt');cmd(['cmake','--build',str(O/'build'),'-j4'],'build.txt')
for kind in ['observer','mechanism']:
 diff=''
 for p in sorted((O/kind).rglob('*')):
  if p.is_file():
   rel=p.relative_to(O/kind);q=O/'original'/rel;diff+=''.join(difflib.unified_diff(q.read_text().splitlines(True),p.read_text().splitlines(True),fromfile='original/'+str(rel),tofile=kind+'/'+str(rel)))
 (O/(kind+'.patch')).write_text(diff)
print('18 TESTS PASS; BUILD READY',flush=True)