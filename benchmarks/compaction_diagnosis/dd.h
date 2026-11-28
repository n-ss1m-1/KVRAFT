#pragma once
#include <cstdint>
#ifdef PROBE
#include <atomic>
#include <chrono>
#include <fstream>
#include <cstdlib>
namespace dd {
enum M { Tables,BloomChecks,BloomRejects,Index,IndexItems,Cache,Hits,Misses,Usage,Load,BlockBytes,BlockGet,Scanned,QueueLock,QueueWait,GroupSize,FirstLock,Backpressure,Rotate,Add,WriteApi,CompactTask,MergeBuild,OutputWrite,CompactReaders,PublishWait,PublishHold,Manifest,Cleanup,FlushTask,FlushBuild,FlushWriter,FlushPublishWait,FlushPublishHold,FlushManifest,Count};
inline const char* names[]={"tables","bloom_checks","bloom_rejects","index","index_items","cache_get","cache_hits","cache_misses","cache_usage","block_load","block_bytes","block_get","scanned","queue_lock","queue_cv","group_requests","first_db_lock","backpressure_cv","rotate","memtable_add","write_api","compact_task","merge_and_build_including_writer","output_write_sync_rename_dirsync","compact_readers","compact_publish_wait","compact_publish_hold","compact_manifest","old_file_cleanup","flush_task","flush_build","flush_writer","flush_publish_wait","flush_publish_hold","flush_manifest"};
struct Cell {uint64_t count=0,units=0,ns=0,timed=0;};
struct Event{uint64_t a,b;int metric,phase;};
struct Slot{Cell cells[8][Count]{};Event events[8192]{};unsigned used=0,dropped=0;};
inline Slot slots[8];inline std::atomic<unsigned> next{0},overflow{0};inline thread_local Slot* own=nullptr;inline thread_local bool registered=false;inline thread_local int phase=0;inline thread_local bool sample=false;inline thread_local uint64_t requests=0;
inline Slot* slot() noexcept{if(!registered){registered=true;auto i=next.fetch_add(1);if(i<8)own=&slots[i];else ++overflow;}return own;}
inline uint64_t now()noexcept{return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void count(M m,uint64_t n=1)noexcept{if(auto*s=slot()){auto&c=s->cells[phase][m];++c.count;c.units+=n;}}
inline void record(M m,uint64_t a,uint64_t b,bool timed,bool event)noexcept{if(auto*s=slot()){auto&c=s->cells[phase][m];++c.count;c.ns+=b-a;c.timed+=timed;if(event&&timed){if(s->used<8192)s->events[s->used++]={a,b,m,phase};else ++s->dropped;}}}
struct Phase{int old;Phase(int p):old(phase){phase=p;}~Phase(){phase=old;}};
struct Query{bool old;Query():old(sample){sample=(requests++%32==0);}~Query(){sample=old;}};
struct Timer{M m;uint64_t start=0;bool on,event;Timer(M n,bool query=false):m(n),on(!query||sample),event(!query){if(on)start=now();}~Timer(){record(m,start,on?now():0,on,event);}};
template<class F>decltype(auto) measure(M m,F&& f){Timer t(m);return f();}
inline void dump(const char* file){std::ofstream f(file);f<<"{\"slots\":"<<next<<",\"overflow\":"<<overflow<<",\"cells\":[";bool first=true;for(unsigned i=0;i<8;++i)for(int p=0;p<8;++p)for(int m=0;m<Count;++m){auto&c=slots[i].cells[p][m];if(!c.count)continue;if(!first)f<<',';first=false;f<<"{\"slot\":"<<i<<",\"phase\":"<<p<<",\"metric\":\""<<names[m]<<"\",\"count\":"<<c.count<<",\"units\":"<<c.units<<",\"ns\":"<<c.ns<<",\"timed\":"<<c.timed<<'}';}f<<"],\"events\":[";first=true;for(unsigned i=0;i<8;++i)for(unsigned j=0;j<slots[i].used;++j){auto&e=slots[i].events[j];if(!first)f<<',';first=false;f<<"{\"slot\":"<<i<<",\"phase\":"<<e.phase<<",\"metric\":\""<<names[e.metric]<<"\",\"start_ns\":"<<e.a<<",\"end_ns\":"<<e.b<<'}';}unsigned dropped=0;for(auto&s:slots)dropped+=s.dropped;f<<"],\"events_dropped\":"<<dropped<<"}\n";}
}
#define DD_SCOPE(m) dd::Timer dd_timer_##m(dd::m)
#define DD_QUERY(m) dd::Timer dd_timer_##m(dd::m,true)
#define DD_COUNT(m,n) dd::count(dd::m,n)
#define DD_TIME(m,expr) dd::measure(dd::m,[&]() -> decltype(auto){return (expr);})
#define DD_PHASE(n) dd::Phase dd_phase(n)
#else
#define DD_SCOPE(m) ((void)0)
#define DD_QUERY(m) ((void)0)
#define DD_COUNT(m,n) ((void)0)
#define DD_TIME(m,expr) (expr)
#define DD_PHASE(n) ((void)0)
#endif
