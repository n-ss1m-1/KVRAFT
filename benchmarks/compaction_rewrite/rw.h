#pragma once
#include <chrono>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include "lsm/sstable.h"
#include "lsm/manifest.h"
namespace rw {
inline uint64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct File{TableMeta m;uint64_t records=0,tombs=0;};
struct Event{uint64_t id=0,request=0,budget=0,start=0,selected=0,buildStart=0,buildEnd=0,wait=0,locked=0,published=0,unlock=0,end=0;uint32_t level=0;std::vector<File> source,target,output;std::vector<uint64_t> beforeN,beforeB,afterN,afterB;std::vector<std::pair<uint64_t,uint64_t>> writers;};
inline std::vector<Event> tasks;inline std::vector<Event> flushes;inline thread_local Event* active=nullptr;
struct Task:Event{Task(uint64_t req,uint32_t l,uint64_t b){id=tasks.size()+1;request=req;level=l;budget=b;start=now();active=this;}~Task(){end=now();tasks.push_back(std::move(*this));active=nullptr;}};
struct WriteScope{uint64_t t=now();~WriteScope(){if(active)active->writers.push_back({t,now()});}};
inline File file(const TableMeta&m){return {m,0,0};}
inline void scan(File&f,const std::string&p){SSTableReader r(p);for(auto it=r.Begin();it.Valid();it.Next()){++f.records;f.tombs+=it.Key().type==ValueType::kDeletion;}}
template<class L>void levels(const L&ls,std::vector<uint64_t>&n,std::vector<uint64_t>&b){for(auto&l:ls){n.push_back(l.size());uint64_t bytes=0;for(auto&s:l)bytes+=s.meta.fileSize;b.push_back(bytes);}}
inline void nums(std::ostream&o,const std::vector<uint64_t>&a){o<<'[';for(size_t i=0;i<a.size();++i){if(i)o<<',';o<<a[i];}o<<']';}
inline void files(std::ostream&o,const std::vector<File>&a){o<<'[';for(size_t i=0;i<a.size();++i){if(i)o<<',';auto&f=a[i];o<<"{\"id\":"<<f.m.fileNumber<<",\"level\":"<<f.m.level<<",\"bytes\":"<<f.m.fileSize<<",\"smallest\":\""<<f.m.smallest.user_key<<"\",\"largest\":\""<<f.m.largest.user_key<<"\",\"records\":"<<f.records<<",\"tombstones\":"<<f.tombs<<'}';}o<<']';}
inline void dump(const std::string&p){std::ofstream o(p);auto emit=[&](const Event&e,const char*kind){o<<"{\"kind\":\""<<kind<<"\",\"id\":"<<e.id<<",\"manual_request\":"<<e.request<<",\"level\":"<<e.level<<",\"source_budget\":"<<e.budget;
#define FIELD(x) o<<",\"" #x "_ns\":"<<e.x;
FIELD(start) FIELD(selected) FIELD(buildStart) FIELD(buildEnd) FIELD(wait) FIELD(locked) FIELD(published) FIELD(unlock) FIELD(end)
#undef FIELD
 o<<",\"source\":";files(o,e.source);o<<",\"target\":";files(o,e.target);o<<",\"output\":";files(o,e.output);o<<",\"before_count\":";nums(o,e.beforeN);o<<",\"before_bytes\":";nums(o,e.beforeB);o<<",\"after_count\":";nums(o,e.afterN);o<<",\"after_bytes\":";nums(o,e.afterB);o<<",\"writer_intervals\":[";for(size_t i=0;i<e.writers.size();++i){if(i)o<<',';o<<'['<<e.writers[i].first<<','<<e.writers[i].second<<']';}o<<"]}\n";};for(auto&e:tasks)emit(e,"compaction");for(auto&e:flushes)emit(e,"flush");}
}