from pathlib import Path
import shutil,subprocess
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/diagnostics-20261116-followup';o.mkdir(exist_ok=False)
shutil.copytree(r/'src/lsm',o/'source-before/lsm');shutil.copytree(r/'benchmarks/diagnostics',o/'harness-before',ignore=shutil.ignore_patterns('__pycache__'))
(o/'git-before.txt').write_text(subprocess.check_output(['git','status','--short'],cwd=r,text=True))
p=r/'src/lsm/include/lsm/diagnostics.h';s=p.read_text().replace('TaskLockWait, MetricCount','TaskLockWait, BloomFound, BloomNotFound, CrossBlocks, FoundBlocks, NotFoundBlocks, MetricCount').replace('"task_lock_wait"','"task_lock_wait","bloom_pass_found","bloom_pass_not_found","same_table_extra_blocks","found_table_blocks","not_found_table_blocks"')
pos=s.index('inline void dump(')
s=s[:pos]+'''struct TaskData {uint64_t number=0,bytes=0,start=0,released=0,end=0;int origin=0;Cell metrics[MetricCount]{};};
inline TaskData tasks[2048];inline unsigned tasks_used=0,tasks_dropped=0;
struct TaskTrace {
 TaskData* data=nullptr;Cell before[MetricCount]{};
 TaskTrace() noexcept {if(tasks_used<2048){data=&tasks[tasks_used++];data->start=now();data->origin=phase;if(auto*s=slot())for(int m=0;m<MetricCount;++m)before[m]=s->cells[phase][operation][m];}else ++tasks_dropped;}
 void file(uint64_t n,uint64_t b) noexcept {if(data){data->number=n;data->bytes=b;}}
 void release() noexcept {if(data)data->released=now();}
 ~TaskTrace(){if(data){data->end=now();if(auto*s=slot())for(int m=0;m<MetricCount;++m){auto c=s->cells[phase][operation][m];c.count-=before[m].count;c.ns-=before[m].ns;data->metrics[m]=c;}}}
};
''' +s[pos:]
s=s.replace('out<<"]}\\n";','''out<<"],\\"tasks_dropped\\":"<<tasks_dropped<<",\\"tasks\\":[";
 for(unsigned i=0;i<tasks_used;++i){auto&t=tasks[i];if(i)out<<',';out<<"{\\"file\\":"<<t.number<<",\\"bytes\\":"<<t.bytes<<",\\"origin\\":"<<t.origin<<",\\"before_release_ns\\":"<<t.released-t.start<<",\\"after_release_ns\\":"<<t.end-t.released<<",\\"metrics\\":[";bool first_metric=true;for(int m=0;m<MetricCount;++m)if(t.metrics[m].count){if(!first_metric)out<<',';first_metric=false;out<<"{\\"metric\\":\\""<<names[m]<<"\\",\\"count\\":"<<t.metrics[m].count<<",\\"ns\\":"<<t.metrics[m].ns<<'}';}out<<"]}";}out<<"]}\\n";''')
p.write_text(s)
p=r/'src/lsm/lib/db.cc';s=p.read_text().replace('lsm_diag::Background);\n#endif','lsm_diag::Background);\n    lsm_diag::TaskTrace diagnosticTask;\n#endif');s=s.replace('    // build TableMeta\n','    // build TableMeta\n#ifdef LSM_DIAGNOSTICS\n    diagnosticTask.file(fileNumber, fileSize);\n#endif\n');s=s.replace('        immutable_.reset();','        immutable_.reset();\n#ifdef LSM_DIAGNOSTICS\n        diagnosticTask.release();\n#endif');p.write_text(s)
p=r/'src/lsm/lib/sstable.cc';s=p.read_text();a=s.index('LookupResult SSTableReader::Get');b=s.index('std::string SSTableReader::ReadBlock',a);part=s[a:b]
part=part.replace('    // 3. 构造target','''#ifdef LSM_DIAGNOSTICS
    struct QueryOutcome {LookupResult& result;uint64_t blocks=0;int exceptions=std::uncaught_exceptions();~QueryOutcome(){if(std::uncaught_exceptions()!=exceptions)return;bool found=result.state!=LookupState::kNotFound;lsm_diag::count(found?lsm_diag::BloomFound:lsm_diag::BloomNotFound);lsm_diag::count(found?lsm_diag::FoundBlocks:lsm_diag::NotFoundBlocks,blocks);if(blocks>1)lsm_diag::count(lsm_diag::CrossBlocks,blocks-1);}} diagnosticQuery{result};
#endif
    // 3. 构造target''').replace('        const auto& idx = index_[blockIndex];','''        const auto& idx = index_[blockIndex];
#ifdef LSM_DIAGNOSTICS
        ++diagnosticQuery.blocks;
#endif''')
s=s[:a]+part+s[b:];p.write_text(s)
# Correct close in legacy query harness too: write path closes before rss, validation, sorting, serialization.
p=r/'benchmarks/diagnostics/diagnose.cc';s=p.read_text().replace('total_seconds=sec(fe-begin);peak=rss();','total_seconds=sec(fe-begin);{lsm_diag::Context closing(lsm_diag::Close,lsm_diag::Other);auto cb=Clock::now();db.reset();close_seconds=sec(Clock::now()-cb);}peak=rss();');s=s.replace('{lsm_diag::Context ctx(lsm_diag::Close,lsm_diag::Other);auto b=Clock::now();db.reset();close_seconds=sec(Clock::now()-b);}','if(db){lsm_diag::Context ctx(lsm_diag::Close,lsm_diag::Other);auto b=Clock::now();db.reset();close_seconds=sec(Clock::now()-b);}')
p.write_text(s)
