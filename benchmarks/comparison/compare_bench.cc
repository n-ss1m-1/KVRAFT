#include "lsm/db.h"
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
#include <sys/resource.h>
#include <vector>
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
static double secs(Clock::duration d){return std::chrono::duration<double>(d).count();}
static rusage usage(){rusage r{}; if(getrusage(RUSAGE_SELF,&r))throw std::runtime_error("getrusage"); std::ifstream status("/proc/self/status"); std::string line; bool found=false; while(std::getline(status,line)) if(line.rfind("VmHWM:",0)==0){r.ru_maxrss=std::stol(line.substr(6));found=true;break;} if(!found)throw std::runtime_error("VmHWM unavailable"); return r;}
static double cpu(rusage r){return r.ru_utime.tv_sec+r.ru_stime.tv_sec+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1e6;}
static std::string key(size_t i){auto s=std::to_string(i);return std::string(16-s.size(),'0')+s;}
static std::string value(size_t version){auto s=std::to_string(version);return s+std::string(128-s.size(),'v');}
struct Op{unsigned type;size_t k;std::string v;};
int main(int argc,char**argv)try{
 if(argc!=8)throw std::runtime_error("DIR MODE N WAL THRESHOLD SEED LATENCY_CSV");
 fs::path dir=argv[1];std::string mode=argv[2];size_t n=std::stoull(argv[3]);
 DBOptions opt;opt.enableWal=std::stoi(argv[4]);opt.memTableSizeLimit=std::stoull(argv[5]);
 if(!n||fs::exists(dir))throw std::runtime_error("N positive, DIR must be new");
 std::mt19937_64 rng(std::stoull(argv[6]));
 std::vector<std::string> keys;for(size_t i=0;i<n;++i)keys.push_back(key(i));
 std::vector<std::optional<std::string>> expected(n);
 auto db=std::make_unique<DB>(dir.string(),opt);
 if(mode=="mixed50"){
  for(size_t i=0;i<n;++i){db->Put(keys[i],value(0));expected[i]=value(0);}
  db->Flush();
 }else if(mode!="random")throw std::runtime_error("unknown mode");
 std::vector<size_t> order(n);std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),rng);
 std::vector<Op> trace;trace.reserve(n);
 for(size_t i=0;i<n;++i){
  unsigned type=1;size_t k=order[i];
  if(mode=="mixed50"){k=rng()%n;auto p=rng()%100;type=p<50?0:(p<60?2:1);}
  trace.push_back({type,k,type==1?value(i+1):std::string()});
 }
 // Preallocate harness storage so allocations do not perturb request timing.
 std::vector<double> latency(n);std::vector<std::optional<std::string>> reads(n);
 auto begin_cpu=usage();auto begin=Clock::now();
 size_t gets=0,puts=0,deletes=0;
 for(size_t i=0;i<n;++i){auto &op=trace[i];auto start=Clock::now();
  if(op.type==0)reads[i]=db->Get(keys[op.k]);
  else if(op.type==1)db->Put(keys[op.k],op.v);
  else db->Delete(keys[op.k]);
  auto end=Clock::now();latency[i]=secs(end-start)*1e6;
 }
 auto requested=Clock::now();auto request_usage=usage();
 auto flush_begin=Clock::now();db->Flush();auto completed=Clock::now();auto completed_usage=usage();
 // All validation is after both timing boundaries. Verify intermediate reads via replayed oracle.
 for(size_t i=0;i<n;++i){const auto &op=trace[i];
  if(op.type==0){++gets;if(reads[i]!=expected[op.k])throw std::runtime_error("trace read mismatch");}
  else if(op.type==1){++puts;expected[op.k]=op.v;}
  else {++deletes;expected[op.k].reset();}
 }
 auto verify=[&](){for(size_t i=0;i<n;++i)if(db->Get(keys[i])!=expected[i])throw std::runtime_error("final data mismatch");
  if(db->Get(std::string(16,'x')))throw std::runtime_error("missing key mismatch");};
 verify();db.reset();
 // Reopen with WAL disabled: validate final Flush persisted everything in SSTables, independent of WAL replay.
 DBOptions reopen=opt;reopen.enableWal=false;db=std::make_unique<DB>(dir.string(),reopen);verify();db.reset();
 size_t tables=0,wals=0;uintmax_t disk=0,walbytes=0;
 for(auto&e:fs::directory_iterator(dir))if(e.is_regular_file()){
  disk+=e.file_size();if(e.path().extension()==".sst")++tables;
  if(e.path().extension()==".wal"||e.path().filename()=="wal.log"){++wals;walbytes+=e.file_size();}
 }
 if(tables<3)throw std::runtime_error("fixture did not produce multiple flushes");
 std::ofstream raw(argv[7]);if(!raw)throw std::runtime_error("latency output failed");
 raw<<"operation,type,latency_us\n"<<std::setprecision(12);
 for(size_t i=0;i<n;++i)raw<<i<<','<<trace[i].type<<','<<latency[i]<<'\n';
 std::sort(latency.begin(),latency.end());auto p=[&](double q){return latency[static_cast<size_t>(std::ceil(n*q))-1];};
 double request_s=secs(requested-begin),total_s=secs(completed-begin);
 std::cout<<std::setprecision(12)<<"{\"request_seconds\":"<<request_s<<",\"final_flush_seconds\":"<<secs(completed-flush_begin)
 <<",\"total_with_flush_seconds\":"<<total_s<<",\"request_ops_per_second\":"<<n/request_s<<",\"total_ops_per_second\":"<<n/total_s
 <<",\"p50_us\":"<<p(.5)<<",\"p95_us\":"<<p(.95)<<",\"p99_us\":"<<p(.99)<<",\"max_us\":"<<latency.back()
 <<",\"peak_rss_request_kib\":"<<request_usage.ru_maxrss<<",\"peak_rss_through_flush_kib\":"<<completed_usage.ru_maxrss
 <<",\"request_cpu_seconds\":"<<cpu(request_usage)-cpu(begin_cpu)<<",\"total_cpu_seconds\":"<<cpu(completed_usage)-cpu(begin_cpu)
 <<",\"sstables_after_close\":"<<tables<<",\"wal_files_after_close\":"<<wals<<",\"wal_bytes_after_close\":"<<walbytes<<",\"disk_bytes_after_close\":"<<disk
 <<",\"gets\":"<<gets<<",\"puts\":"<<puts<<",\"deletes\":"<<deletes<<",\"verified_keys_twice\":"<<n<<",\"verified\":true}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
