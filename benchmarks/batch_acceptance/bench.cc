#include "lsm/db.h"
#include "lsm/write_batch.h"
#include "lsm/wal.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <sys/resource.h>
using Clock=std::chrono::steady_clock;
static uint64_t ns(Clock::time_point a,Clock::time_point b){return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();}
struct Counters { uint64_t data_sync=0,new_sync=0,bytes=0,records=0; };
static thread_local int stage=0;
static thread_local bool dirty=false;
static Counters counters[5];
#ifdef MECHANISM
#include <sys/stat.h>
static bool iswal(int fd){
 char path[64],target[4096]; std::snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
 auto n=readlink(path,target,sizeof(target));if(n<4)return false;
 return std::string_view(target,n).substr(n-4)==".wal";
}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" ssize_t __wrap_write(int fd,const void* p,size_t n){
 auto result=__real_write(fd,p,n);
 if(stage>0 && result>0 && iswal(fd)){counters[stage].bytes+=result;++counters[stage].records;}
 return result;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
 bool wal=stage>0 && iswal(fd);struct stat st{};if(wal && fstat(fd,&st))wal=false;
 int result=__real_fsync(fd);
 if(wal && result==0){if(st.st_size>0)++counters[stage].data_sync;else ++counters[stage].new_sync;}
 return result;
}
#endif
struct Op {std::string key,value;bool del=false;};
static void verify(DB& db,const std::map<std::string,std::optional<std::string>>& oracle) {
 for(auto& p:oracle)if(db.Get(p.first)!=p.second)throw std::runtime_error("oracle mismatch");
}
int main(int argc,char**argv) {
 try {
 if(argc!=9)throw std::runtime_error("args: directory wal batch n timed mode workload prefix");
 std::string dir=argv[1],mode=argv[6],work=argv[7],prefix=argv[8];
 bool wal=std::stoi(argv[2]),timed=std::stoi(argv[5]); size_t batch=std::stoul(argv[3]),n=std::stoul(argv[4]);
 if(std::filesystem::exists(dir))throw std::runtime_error("refuse existing directory");
 std::vector<size_t> ids(n);std::iota(ids.begin(),ids.end(),0); std::mt19937_64 rng(20261115);std::shuffle(ids.begin(),ids.end(),rng);
 std::vector<Op> trace;trace.reserve(n);std::map<std::string,std::optional<std::string>> oracle;
 for(size_t i=0;i<n;++i){ char key[32];size_t id=work=="mixed"?ids[i]/4:ids[i];std::snprintf(key,sizeof(key),"%016zu",id);
 Op op{key,std::string(128,char('a'+i%26)),work=="mixed" && i%5==0};
 if(work=="mixed" && i%8==1)op.key=trace.back().key;
 oracle[op.key]=op.del?std::optional<std::string>{}:std::optional<std::string>{op.value};trace.push_back(std::move(op));}
 // All allocation for trace, oracle, latency capacity is outside timed submission.
 std::vector<uint64_t> lat;lat.reserve((n+std::max<size_t>(batch,1)-1)/std::max<size_t>(batch,1));
 DBOptions opt;opt.enableWal=wal;opt.memTableSizeLimit=1048576;opt.BlockCacheCapacity=0;
 auto db=std::make_unique<DB>(dir,opt); uint64_t assembly=0,clear=0,api_sum=0;size_t completed=0; WriteBatch wb;
 stage=1; auto start=Clock::now();
 for(size_t i=0;i<n;){
 size_t count=batch?std::min(batch,n-i):1;
 if(batch){auto a=timed?Clock::now():Clock::time_point{};
 for(size_t j=0;j<count;++j){auto&o=trace[i+j];if(o.del)wb.Delete(o.key);else wb.Put(o.key,o.value);}
 if(timed)assembly+=ns(a,Clock::now());}
 auto a=timed?Clock::now():Clock::time_point{};
 try {if(batch)db->Write(wb);else if(trace[i].del)db->Delete(trace[i].key);else db->Put(trace[i].key,trace[i].value);}
 catch(...){if(timed)lat.push_back(ns(a,Clock::now()));throw;}
 if(timed){auto dt=ns(a,Clock::now());lat.push_back(dt);api_sum+=dt;}
 if(batch){auto a=timed?Clock::now():Clock::time_point{}; wb.Clear();if(timed)clear+=ns(a,Clock::now());}
 i+=count;completed+=count;
 }
 auto submitted=Clock::now();
 if(mode=="recovery"){
   stage=3; db.reset(); stage=4; opt.enableWal=true;
   {DB reopened(dir,opt);verify(reopened,oracle);}
   std::cout<<"{\"status\":\"ok\",\"recovery_verified\":true,\"n\":"<<n<<",\"batch\":"<<batch<<"}\n";return 0;
 }
 stage=2; db->Flush();auto flushed=Clock::now();
 // Timed runs destroy immediately. Independent check/mechanism runs additionally inspect the live DB.
 if(mode=="check")verify(*db,oracle);
 stage=3;auto close_start=mode=="check"?Clock::now():flushed;db.reset();auto closed=Clock::now();rusage usage{};getrusage(RUSAGE_SELF,&usage);stage=4;
 opt.enableWal=false;{DB reopened(dir,opt);verify(reopened,oracle);}

 size_t tables=0,table_bytes=0;for(auto&e:std::filesystem::directory_iterator(dir))if(e.path().extension()==".sst"){++tables;table_bytes+=e.file_size();}
 auto sorted=lat;std::sort(sorted.begin(),sorted.end());auto q=[&](double x){return sorted.empty()?0:sorted[std::min(sorted.size()-1,size_t(std::ceil(x*sorted.size()))-1)];};
 
 std::ofstream latency(prefix+".latency.csv");latency<<"submission,ns\n";for(size_t i=0;i<lat.size();++i)latency<<i<<","<<lat[i]<<"\n";
 std::cout<<"{\"status\":\"ok\",\"n\":"<<n<<",\"completed\":"<<completed<<",\"wal\":"<<wal<<",\"batch\":"<<batch<<",\"timed\":"<<timed<<",\"submissions\":"<<(n+std::max<size_t>(batch,1)-1)/std::max<size_t>(batch,1)
 <<",\"assembly_ns\":"<<assembly<<",\"clear_ns\":"<<clear<<",\"api_sum_ns\":"<<api_sum<<",\"submit_ns\":"<<ns(start,submitted)<<",\"flush_ns\":"<<ns(submitted,flushed)<<",\"total_ns\":"<<ns(start,flushed)<<",\"close_ns\":"<<ns(close_start,closed)
 <<",\"p50_ns\":"<<q(.5)<<",\"p95_ns\":"<<q(.95)<<",\"p99_ns\":"<<q(.99)<<",\"max_ns\":"<<q(1)<<",\"rss_kib\":"<<usage.ru_maxrss<<",\"tables\":"<<tables<<",\"sst_bytes\":"<<table_bytes<<",\"live_verified\":"<<(mode=="check")<<",\"reopen_verified\":true,\"mechanism\":[";
 for(int i=0;i<5;++i){if(i)std::cout<<",";auto c=counters[i];std::cout<<"{\"data_sync\":"<<c.data_sync<<",\"new_sync\":"<<c.new_sync<<",\"bytes\":"<<c.bytes<<",\"records\":"<<c.records<<"}";}
 std::cout<<"]}\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";std::cout<<"{\"status\":\"failed\"}\n";return 1;}
}
