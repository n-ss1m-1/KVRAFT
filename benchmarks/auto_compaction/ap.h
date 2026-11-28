#pragma once
#include <chrono>
#include <cstdint>
#include <fstream>
namespace ap {
inline uint64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Event{uint64_t start=0,end=0,published=0,input_bytes=0,output_bytes=0,input_records=0,output_records=0,source=0,target=0,outputs=0;uint32_t level=0;};
inline Event events[4096];inline unsigned used=0,dropped=0;
inline uint64_t flushBytes=0,flushCount=0;inline thread_local uint64_t bpNs=0,bpCount=0,tables=0;
struct Task:Event{Task(uint32_t l){level=l;start=now();}~Task(){end=now();if(used<4096)events[used++]=*this;else dropped++;}};
inline void dump(const std::string& path){std::ofstream f(path);f<<"level,start_ns,published_ns,end_ns,source,target,outputs,input_bytes,output_bytes,input_records,output_records\n";for(unsigned i=0;i<used;i++){auto&e=events[i];f<<e.level<<','<<e.start<<','<<e.published<<','<<e.end<<','<<e.source<<','<<e.target<<','<<e.outputs<<','<<e.input_bytes<<','<<e.output_bytes<<','<<e.input_records<<','<<e.output_records<<'\n';}}
}
