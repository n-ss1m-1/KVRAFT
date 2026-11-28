#include "lsm/db.h"
#include "lsm/write_batch.h"
#include "lsm/wal.h"
#include "lsm/sstable.h"
#include "lsm/manifest.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>
#include <sys/resource.h>
#include <sys/stat.h>
using Clock=std::chrono::steady_clock;
static uint64_t ns(Clock::time_point a,Clock::time_point b){return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();}
struct Counts {uint64_t sync=0,new_sync=0,bytes=0,records=0,dirs=0,other=0,partial=0,pending=0,hist[65]{};};
static Counts counts[11][5]; // 0..7 writers, 8 main, 9 reader, 10 background; no concurrent slot writers.
static thread_local int slot=10,phase=0;
#ifdef MECHANISM
static bool walfile(int fd){char p[64],dest[4096];std::snprintf(p,sizeof(p),"/proc/self/fd/%d",fd);auto n=readlink(p,dest,sizeof(dest));return n>=4 && std::string_view(dest,n).substr(n-4)==".wal";}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" ssize_t __wrap_write(int fd,const void* p,size_t n){
 auto result=__real_write(fd,p,n);
 if(result>0 && walfile(fd)){
 auto& c=counts[slot][phase];c.bytes+=result;
 if(size_t(result)==n && n>=8 && (lsm::DecodeFixed32(static_cast<const char*>(p))&0x7fffffffU)+8==n){++c.records;++c.pending;}else ++c.partial;
 }return result;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
 bool wal=walfile(fd);struct stat st{};bool statok=fstat(fd,&st)==0;auto result=__real_fsync(fd);
 if(result==0){auto&c=counts[slot][phase];if(wal && statok){if(st.st_size){++c.sync;++c.hist[std::min<uint64_t>(64,c.pending)];c.pending=0;}else ++c.new_sync;}else if(statok && S_ISDIR(st.st_mode))++c.dirs;else ++c.other;}
 return result;
}
#endif
struct Gate{
 std::mutex m;std::condition_variable cv;size_t ready=0;bool go=false;
 void wait(){std::unique_lock<std::mutex>l(m);++ready;cv.notify_all();cv.wait(l,[&]{return go;});}
 Clock::time_point release(size_t n){std::unique_lock<std::mutex>l(m);cv.wait(l,[&]{return ready==n;});auto t=Clock::now();go=true;cv.notify_all();return t;}
};
struct Op{std::string key,value;};
struct Worker{std::vector<uint64_t> lat;uint64_t assembly=0,calls=0,ops=0;std::exception_ptr error;Clock::time_point start,end;};
static void check(DB&db,const std::vector<Op>&ops){for(auto&o:ops)if(db.Get(o.key)!=std::optional<std::string>(o.value))throw std::runtime_error("DB oracle mismatch "+o.key);}
static void check_tables(const std::string&dir,const std::vector<Op>&ops){
 auto manifest=Manifest::Load(dir+"/MANIFEST");std::vector<std::unique_ptr<SSTableReader>> tables;
 for(auto&m:manifest.Tables()){char name[32];std::snprintf(name,sizeof(name),"/%06llu.sst",(unsigned long long)m.fileNumber);tables.push_back(std::make_unique<SSTableReader>(dir+name));}
 for(auto&o:ops){bool found=false;for(auto it=tables.rbegin();it!=tables.rend();++it){auto v=(*it)->Get(manifest.FlushedSequence(),o.key);if(v.state==LookupState::kValue){if(v.value!=o.value)throw std::runtime_error("SST value mismatch");found=true;break;}if(v.state==LookupState::kDeleted)break;}if(!found)throw std::runtime_error("SST oracle missing");}
}
static uint64_t quantile(std::vector<uint64_t> a,double q){if(a.empty())return 0;std::sort(a.begin(),a.end());return a[std::min(a.size()-1,size_t(std::ceil(q*a.size()))-1)];}
int main(int argc,char**argv){
 try{
 // directory wal threads requests-limit byte-budget batch total timed readers mode prefix
 if(argc!=12)throw std::runtime_error("expected 11 arguments");
 slot=8;std::string dir=argv[1],mode=argv[10],prefix=argv[11];
 bool wal=std::stoi(argv[2]),timed=std::stoi(argv[8]);size_t nt=std::stoul(argv[3]),limit=std::stoul(argv[4]),budget=std::stoul(argv[5]),batch=std::stoul(argv[6]),n=std::stoul(argv[7]),nr=std::stoul(argv[9]);
 if(nt>8 || nr>1 || n%nt || !nt || std::filesystem::exists(dir))throw std::runtime_error("invalid harness configuration");
 std::vector<Op> ops(n);std::vector<std::vector<size_t>> traces(nt);
 for(size_t i=0;i<n;++i){char k[32];std::snprintf(k,sizeof(k),"%016zu",i);ops[i]={k,std::string(128,char('a'+i%26))};}
 for(size_t t=0;t<nt;++t){auto&ids=traces[t];ids.resize(n/nt);std::iota(ids.begin(),ids.end(),t*(n/nt));std::mt19937_64 rng(20261115+t);std::shuffle(ids.begin(),ids.end(),rng);}
 std::vector<Op> reads;if(nr){for(size_t i=0;i<64;++i){char k[32];std::snprintf(k,sizeof(k),"r%015zu",i);reads.push_back({k,std::string(128,'R')});}}
 DBOptions opt;opt.enableWal=wal;opt.memTableSizeLimit=1048576;opt.blockCacheCapacity=0;opt.maxGroupRequests=limit;opt.maxGroupBytes=budget;
 auto db=std::make_unique<DB>(dir,opt);
 if(nr){for(auto&o:reads)db->Put(o.key,o.value);db->Flush();check(*db,reads);}
 std::vector<Worker> workers(nt);Worker reader;reader.lat.reserve(n*64); // capped reader latency sample prefix, count continues if exhausted.
 Gate gate;std::atomic<bool> stop{false};std::vector<std::thread> threads;threads.reserve(nt+nr);
 for(size_t t=0;t<nt;++t){auto&w=workers[t];w.lat.reserve((n/nt+std::max<size_t>(batch,1)-1)/std::max<size_t>(batch,1));
 threads.emplace_back([&,t]{slot=t;phase=1;auto&w=workers[t];WriteBatch wb;gate.wait();w.start=Clock::now();
 try{for(size_t i=0;i<traces[t].size();){size_t cnt=batch?std::min(batch,traces[t].size()-i):1;
 if(batch){auto a=timed?Clock::now():Clock::time_point{};for(size_t j=0;j<cnt;++j){auto&o=ops[traces[t][i+j]];wb.Put(o.key,o.value);}if(timed)w.assembly+=ns(a,Clock::now());}
 auto a=timed?Clock::now():Clock::time_point{};
 if(batch)db->Write(wb);else {auto&o=ops[traces[t][i]];db->Put(o.key,o.value);}
 if(timed)w.lat.push_back(ns(a,Clock::now()));if(batch)wb.Clear();++w.calls;w.ops+=cnt;i+=cnt;
 }}catch(...){w.error=std::current_exception();}w.end=Clock::now();
 });}
 if(nr)threads.emplace_back([&]{slot=9;phase=1;gate.wait();reader.start=Clock::now();try{while(!stop.load(std::memory_order_relaxed)){auto&o=reads[reader.calls%reads.size()];auto a=Clock::now();auto got=db->Get(o.key);auto b=Clock::now();if(reader.lat.size()<reader.lat.capacity())reader.lat.push_back(ns(a,b));if(got!=std::optional<std::string>(o.value))throw std::runtime_error("reader mismatch");++reader.calls;}}catch(...){reader.error=std::current_exception();}reader.end=Clock::now();});
 auto start=gate.release(nt+nr);for(size_t t=0;t<nt;++t)threads[t].join();stop=true;if(nr)threads[nt].join();
 auto end=workers[0].end;auto first=workers[0].start;for(auto&w:workers){end=std::max(end,w.end);first=std::min(first,w.start);if(w.error)std::rethrow_exception(w.error);}if(reader.error)std::rethrow_exception(reader.error);
 auto drained=Clock::now();phase=2;auto flush_start=Clock::now();
 if(mode!="recovery")db->Flush();auto flushed=Clock::now();phase=3;db.reset();auto closed=Clock::now();
 rusage usage{};getrusage(RUSAGE_SELF,&usage);phase=4;
 if(mode=="recovery"){opt.enableWal=true;DB recovered(dir,opt);check(recovered,ops);}else{check_tables(dir,ops);opt.enableWal=false;DB reopened(dir,opt);check(reopened,ops);check(reopened,reads);}
 std::vector<uint64_t> lat;uint64_t calls=0,done=0,assembly=0;for(auto&w:workers){lat.insert(lat.end(),w.lat.begin(),w.lat.end());calls+=w.calls;done+=w.ops;assembly+=w.assembly;}
 std::ofstream csv(prefix+".latency.csv");csv<<"thread,submission,ns\n";for(size_t t=0;t<nt;++t)for(size_t j=0;j<workers[t].lat.size();++j)csv<<t<<","<<j<<","<<workers[t].lat[j]<<"\n";
 std::ofstream rcsv(prefix+".read-latency.csv");rcsv<<"submission,ns\n";for(size_t j=0;j<reader.lat.size();++j)rcsv<<j<<","<<reader.lat[j]<<"\n";
 size_t ssts=0,bytes=0;for(auto&e:std::filesystem::directory_iterator(dir))if(e.path().extension()==".sst"){++ssts;bytes+=e.file_size();}
 std::cout<<"{\"status\":\"ok\",\"operations\":"<<done<<",\"calls\":"<<calls<<",\"assembly_ns\":"<<assembly<<",\"request_ns\":"<<ns(first,end)<<",\"barrier_to_end_ns\":"<<ns(start,end)<<",\"drain_ns\":"<<ns(end,drained)<<",\"flush_ns\":"<<ns(flush_start,flushed)<<",\"total_ns\":"<<ns(start,flushed)<<",\"close_ns\":"<<ns(flushed,closed)<<",\"p50_ns\":"<<quantile(lat,.5)<<",\"p95_ns\":"<<quantile(lat,.95)<<",\"p99_ns\":"<<quantile(lat,.99)<<",\"max_ns\":"<<quantile(lat,1)<<",\"rss_kib\":"<<usage.ru_maxrss<<",\"tables\":"<<ssts<<",\"sst_bytes\":"<<bytes<<",\"reader_calls\":"<<reader.calls<<",\"reader_ns\":"<<(nr?ns(reader.start,reader.end):0)<<",\"reader_sampled\":"<<reader.lat.size()<<",\"reader_p50_ns\":"<<quantile(reader.lat,.5)<<",\"reader_p95_ns\":"<<quantile(reader.lat,.95)<<",\"reader_p99_ns\":"<<quantile(reader.lat,.99)<<",\"reader_max_ns\":"<<quantile(reader.lat,1)<<",\"verified\":true,\"mechanism\":[";
 for(int p=0;p<5;++p){Counts sum;for(int t=0;t<11;++t){auto&c=counts[t][p];sum.sync+=c.sync;sum.new_sync+=c.new_sync;sum.bytes+=c.bytes;sum.records+=c.records;sum.dirs+=c.dirs;sum.other+=c.other;sum.partial+=c.partial;for(int h=0;h<65;++h)sum.hist[h]+=c.hist[h];}if(p)std::cout<<",";std::cout<<"{\"data_sync\":"<<sum.sync<<",\"new_sync\":"<<sum.new_sync<<",\"wal_bytes\":"<<sum.bytes<<",\"records\":"<<sum.records<<",\"directory_sync\":"<<sum.dirs<<",\"other_sync\":"<<sum.other<<",\"partial\":"<<sum.partial<<",\"hist\":[";for(int h=0;h<65;++h){if(h)std::cout<<",";std::cout<<sum.hist[h];}std::cout<<"]}";}
 std::cout<<"]}\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";std::cout<<"{\"status\":\"failed\"}\n";return 1;}
}
