#pragma once
// Diagnostic-only, C++17. One process/session; snapshots only after all writers join.
#include <cstdint>
#ifdef LSM_DIAGNOSTICS
#include <atomic>
#include <chrono>
#include <exception>
#include <ostream>
#include <system_error>
#include <type_traits>
#include <utility>
#endif
namespace lsm_diag {
enum Phase { Setup, Request, FinalFlush, Verify, Reopen, Close, LatestFirst, LatestWarm, OldestFirst, OldestWarm, MissingFirst, MissingWarm, PhaseCount };
enum Op { Other, Put, Delete, Get, Flush, Background, OpCount };
enum Metric { Api, FirstLock, Backpressure, Append, Sync, Add, Rotate, CreateWal,
 NewWalSync, DirectorySync, Build, Writer, ReaderInit, PublishWait, PublishHold, Manifest,
 Cleanup, FlushWait, Join, Tables, BloomChecks, BloomRejects, IndexLookups, Blocks, Bytes,
 ReadBlock, Pread, Crc, Validate, BlockGet, ValidatedRecords, ScannedRecords, ActiveGet, ImmutableGet,
 PreadCalls, PreadBytes, FileNumberWait, FileNumberHold, TaskLockWait, BloomFound, BloomNotFound, CrossBlocks, FoundBlocks, NotFoundBlocks, MetricCount };
#ifdef LSM_DIAGNOSTICS
struct Cell {uint64_t count=0,ns=0,max_ns=0,failed=0,units=0,timed_count=0;};
struct alignas(64) Slot {Cell cells[PhaseCount][OpCount][MetricCount]{};};
inline Slot slots[32];
inline std::atomic<unsigned> next_slot{0}, dropped_threads{0};
inline thread_local Slot* local_slot=nullptr;
inline thread_local bool registered=false;
inline thread_local Phase phase=Setup;
inline thread_local Op operation=Other;
inline thread_local uint64_t block_sequence=0;
inline thread_local bool sample_block=true;
inline Slot* slot() noexcept {
 if(!registered){registered=true;auto i=next_slot.fetch_add(1,std::memory_order_relaxed);
 if(i<32)local_slot=&slots[i];else dropped_threads.fetch_add(1,std::memory_order_relaxed);}
 return local_slot;
}
inline void register_thread() noexcept {(void)slot();}
struct Context {Phase old_phase;Op old_op;Context(Phase p,Op o) noexcept:old_phase(phase),old_op(operation){phase=p;operation=o;}~Context(){phase=old_phase;operation=old_op;}};
inline uint64_t now() noexcept {return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline void record(Metric m,uint64_t ns,bool failed=false,uint64_t units=0,bool timed=true) noexcept {
 if(auto*s=slot()){auto&c=s->cells[phase][operation][m];++c.count;c.timed_count+=timed;c.ns+=ns;if(ns>c.max_ns)c.max_ns=ns;c.failed+=failed;c.units+=units;}}
inline void count(Metric m,uint64_t units=1) noexcept {record(m,0,false,units,false);}
struct Timer {
 Metric m;uint64_t start;int exceptions;bool bad=false,timed;
 Timer(Metric v) noexcept:m(v),start(0),exceptions(std::uncaught_exceptions()),timed(true){
  if(v==ReadBlock)sample_block=(block_sequence++%8)==0;
  if(v==ReadBlock||v==Pread||v==Crc||v==Validate||v==BlockGet)timed=sample_block;
  if(timed)start=now();
 }
 ~Timer(){record(m,timed?now()-start:0,bad||std::uncaught_exceptions()>exceptions,0,timed);}
 void fail()noexcept{bad=true;}
};
template<class F> decltype(auto) measure(Metric m,F&&f){Timer t(m);if constexpr(std::is_same_v<std::invoke_result_t<F>,std::error_code>){auto ec=std::forward<F>(f)();t.bad=static_cast<bool>(ec);return ec;}else return std::forward<F>(f)();}
inline const char* names[]={"api","first_lock","backpressure","wal_append","wal_sync","memtable_add","rotate","create_wal","new_wal_sync","directory_sync","sst_build","sst_writer_write_sync_rename_dirsync","reader_init","publish_wait","publish_hold","manifest_save","wal_cleanup","flush_cv_wait","join","tables_checked","bloom_checks","bloom_rejects","index_lookups","blocks","block_bytes_with_crc","read_block","pread_all","crc","block_construct_validate","block_get","validated_records","scanned_records","active_get","immutable_get","pread_calls","pread_returned_bytes","file_number_wait","file_number_hold","task_lock_wait","bloom_pass_found","bloom_pass_not_found","same_table_extra_blocks","found_table_blocks","not_found_table_blocks"};
struct TaskData {uint64_t number=0,bytes=0,start=0,released=0,end=0;int origin=0;Cell metrics[MetricCount]{};};
inline TaskData tasks[2048];inline unsigned tasks_used=0,tasks_dropped=0;
struct TaskTrace {
 TaskData* data=nullptr;Cell before[MetricCount]{};
 TaskTrace() noexcept {if(tasks_used<2048){data=&tasks[tasks_used++];data->start=now();data->origin=phase;if(auto*s=slot())for(int m=0;m<MetricCount;++m)before[m]=s->cells[phase][operation][m];}else ++tasks_dropped;}
 void file(uint64_t n,uint64_t b) noexcept {if(data){data->number=n;data->bytes=b;}}
 void release() noexcept {if(data)data->released=now();}
 ~TaskTrace(){if(data){data->end=now();if(auto*s=slot())for(int m=0;m<MetricCount;++m){auto c=s->cells[phase][operation][m];c.count-=before[m].count;c.ns-=before[m].ns;data->metrics[m]=c;}}}
};
inline void dump(std::ostream&out){
 out<<"{\"dropped_threads\":"<<dropped_threads.load()<<",\"slots_used\":"<<next_slot.load()<<",\"cells\":[";bool first=true;
 for(unsigned s=0;s<32;++s)for(int p=0;p<PhaseCount;++p)for(int o=0;o<OpCount;++o)for(int m=0;m<MetricCount;++m){auto&c=slots[s].cells[p][o][m];if(!c.count)continue;
 if(!first){out<<',';}first=false;out<<"{\"slot\":"<<s<<",\"phase\":"<<p<<",\"op\":"<<o<<",\"metric\":\""<<names[m]<<"\",\"count\":"<<c.count<<",\"timed_count\":"<<c.timed_count<<",\"ns\":"<<c.ns<<",\"max_ns\":"<<c.max_ns<<",\"failed\":"<<c.failed<<",\"units\":"<<c.units<<'}';}out<<"],\"tasks_dropped\":"<<tasks_dropped<<",\"tasks\":[";
 for(unsigned i=0;i<tasks_used;++i){auto&t=tasks[i];if(i)out<<',';out<<"{\"file\":"<<t.number<<",\"bytes\":"<<t.bytes<<",\"origin\":"<<t.origin<<",\"before_release_ns\":"<<t.released-t.start<<",\"after_release_ns\":"<<t.end-t.released<<",\"metrics\":[";bool first_metric=true;for(int m=0;m<MetricCount;++m)if(t.metrics[m].count){if(!first_metric)out<<',';first_metric=false;out<<"{\"metric\":\""<<names[m]<<"\",\"count\":"<<t.metrics[m].count<<",\"ns\":"<<t.metrics[m].ns<<'}';}out<<"]}";}out<<"]}\n";
}
#else
inline void register_thread() noexcept {}
struct Context {Context(Phase,Op) noexcept {}};
inline void count(Metric,uint64_t=1) noexcept {}
#endif
}
#ifdef LSM_DIAGNOSTICS
#define LSM_TIME(m,expr) lsm_diag::measure(lsm_diag::m,[&]() -> decltype(auto) {return (expr);})
#define LSM_SCOPE(m) lsm_diag::Timer diag_timer_##m(lsm_diag::m)
#define LSM_LOCK_START(name) auto name=lsm_diag::now()
#define LSM_LOCK_END(name,metric) lsm_diag::record(lsm_diag::metric,lsm_diag::now()-name)
#else
#define LSM_TIME(m,expr) (expr)
#define LSM_SCOPE(m) ((void)0)
#define LSM_LOCK_START(name) ((void)0)
#define LSM_LOCK_END(name,metric) ((void)0)
#endif
