#include "lsm/db.h"
#include "lsm/diagnostics.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>
#include <sys/resource.h>
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
static double sec(Clock::duration d){return std::chrono::duration<double>(d).count();}
static long rss(){std::ifstream f("/proc/self/status");std::string l;while(std::getline(f,l))if(l.rfind("VmHWM:",0)==0)return std::stol(l.substr(6));throw std::runtime_error("VmHWM missing");}
static std::string key(size_t i){auto s=std::to_string(i);return std::string(16-s.size(),'0')+s;}
static std::string value(size_t i){auto s=std::to_string(i);return s+std::string(128-s.size(),'v');}
struct Result{std::string name;size_t count;double elapsed,p50,p95,p99,max;};
int main(int argc,char**argv)try{
 if(argc!=9)throw std::runtime_error("DIR MODE N WAL LIMIT TABLES SEED PREFIX");
 fs::path dir=argv[1];std::string mode=argv[2],prefix=argv[8];size_t n=std::stoull(argv[3]),nt=std::stoull(argv[6]);
 DBOptions opt;opt.enableWal=std::stoi(argv[4]);opt.memTableSizeLimit=std::stoull(argv[5]);
 if(!n||fs::exists(dir))throw std::runtime_error("new directory and positive N required");
 lsm_diag::register_thread();
 std::vector<std::string> keys,values;for(size_t i=0;i<n;++i){keys.push_back(key(2*i));values.push_back(value(i));}
 std::vector<size_t> order(n);std::iota(order.begin(),order.end(),0);std::mt19937_64 rng(std::stoull(argv[7]));std::shuffle(order.begin(),order.end(),rng);
 std::vector<double> latency(n);std::vector<std::optional<std::string>> got(n);
 std::ofstream raw(prefix+".latency.csv");raw<<"case,operation,success,latency_us\n"<<std::setprecision(12);
 std::vector<Result> results;
 double flush_seconds=0,close_seconds=0,total_seconds=0;long peak=0;
 auto db=std::make_unique<DB>(dir.string(),opt);
 auto run=[&](const std::string&name,const std::vector<size_t>&queries,bool miss,bool writing){
  // Per-request capture covers only the public call, including exception exit.
  auto phase=lsm_diag::Request;
  if(!writing){if(name=="latest_first")phase=lsm_diag::LatestFirst;else if(name=="latest_warm")phase=lsm_diag::LatestWarm;else if(name=="oldest_first")phase=lsm_diag::OldestFirst;else if(name=="oldest_warm")phase=lsm_diag::OldestWarm;else if(name=="missing_first")phase=lsm_diag::MissingFirst;else phase=lsm_diag::MissingWarm;}
  lsm_diag::Context ctx(phase,writing?lsm_diag::Put:lsm_diag::Get);
  auto begin=Clock::now();
  for(size_t i=0;i<queries.size();++i){size_t k=queries[i];auto start=Clock::now();
   try{if(writing)db->Put(keys[k],values[k]);else got[i]=db->Get(miss?key(2*k+1):keys[k]);}
   catch(...){auto end=Clock::now();raw<<name<<','<<i<<",false,"<<sec(end-start)*1e6<<'\n';throw;}
   auto end=Clock::now();latency[i]=sec(end-start)*1e6;
  }
  auto end=Clock::now();double elapsed=sec(end-begin);
  if(writing){lsm_diag::Context final(lsm_diag::FinalFlush,lsm_diag::Flush);auto fb=Clock::now();db->Flush();auto fe=Clock::now();flush_seconds=sec(fe-fb);total_seconds=sec(fe-begin);{lsm_diag::Context closing(lsm_diag::Close,lsm_diag::Other);auto cb=Clock::now();db.reset();close_seconds=sec(Clock::now()-cb);}peak=rss();}
  // Timing ends before verification/output. Keep query expectations outside API samples.
  {lsm_diag::Context check(lsm_diag::Verify,lsm_diag::Other);
   if(!writing)for(size_t i=0;i<queries.size();++i)if(got[i]!=(miss?std::optional<std::string>{}:std::optional<std::string>{values[queries[i]]}))throw std::runtime_error("query mismatch");}
  for(size_t i=0;i<queries.size();++i)raw<<name<<','<<i<<",true,"<<latency[i]<<'\n';
  auto v=std::vector<double>(latency.begin(),latency.begin()+queries.size());std::sort(v.begin(),v.end());
  auto p=[&](double q){return v[static_cast<size_t>(std::ceil(q*v.size()))-1];};
  results.push_back({name,queries.size(),elapsed,p(.5),p(.95),p(.99),v.back()});
 };
 if(mode=="write"){
  run("put",order,false,true);
 }else if(mode=="query"){
  if(!nt||nt>n)throw std::runtime_error("table count invalid");
  {lsm_diag::Context ctx(lsm_diag::Setup,lsm_diag::Put);
   for(size_t t=0;t<nt;++t){for(size_t i=t;i<n;i+=nt)db->Put(keys[i],values[i]);lsm_diag::Context flush(lsm_diag::Setup,lsm_diag::Flush);db->Flush();}}
  for(auto kind:{std::string("latest"),std::string("oldest"),std::string("missing")}){
   std::vector<size_t> q;for(size_t j=0;j<n;++j){size_t k=order[j];if(kind=="missing")q.push_back(k);else{size_t t=kind=="latest"?nt-1:0;size_t candidates=(n-1-t)/nt+1;q.push_back(t+(k%candidates)*nt);}}
   run(kind+"_first",q,kind=="missing",false);
   run(kind+"_warm",q,kind=="missing",false);
  }
  peak=rss();
 }else throw std::runtime_error("unknown mode");
 // Close immediately after timed work: do not let full validation hide WAL cleanup/join.
 if(db){lsm_diag::Context ctx(lsm_diag::Close,lsm_diag::Other);auto b=Clock::now();db.reset();close_seconds=sec(Clock::now()-b);}
 {lsm_diag::Context ctx(lsm_diag::Reopen,lsm_diag::Get);auto ro=opt;ro.enableWal=false;db=std::make_unique<DB>(dir.string(),ro);for(size_t i=0;i<n;++i)if(db->Get(keys[i])!=std::optional<std::string>{values[i]})throw std::runtime_error("reopen mismatch");db.reset();}
 size_t tables=0;uintmax_t disk=0;for(auto&e:fs::directory_iterator(dir))if(e.is_regular_file()){disk+=e.file_size();tables+=e.path().extension()==".sst";}
 std::ofstream stats(prefix+".stats.json");
#ifdef LSM_DIAGNOSTICS
 lsm_diag::dump(stats);
#else
 stats<<"{\"disabled\":true}\n";
#endif
 std::cout<<std::setprecision(12)<<"{\"success\":true,\"n\":"<<n<<",\"tables\":"<<tables<<",\"disk_bytes\":"<<disk<<",\"final_flush_seconds\":"<<flush_seconds<<",\"total_with_flush_seconds\":"<<total_seconds<<",\"close_seconds\":"<<close_seconds<<",\"peak_rss_kib\":"<<peak<<",\"results\":[";
 bool first=true;for(auto&r:results){if(!first)std::cout<<',';first=false;std::cout<<"{\"name\":\""<<r.name<<"\",\"operations\":"<<r.count<<",\"seconds\":"<<r.elapsed<<",\"ops_per_second\":"<<r.count/r.elapsed<<",\"p50_us\":"<<r.p50<<",\"p95_us\":"<<r.p95<<",\"p99_us\":"<<r.p99<<",\"max_us\":"<<r.max<<'}';}std::cout<<"]}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
