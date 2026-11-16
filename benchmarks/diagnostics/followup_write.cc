#include "lsm/db.h"
#include "lsm/diagnostics.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>
#include <vector>
using Clock=std::chrono::steady_clock;
static double secs(Clock::duration d){return std::chrono::duration<double>(d).count();}
static std::string key(size_t k){auto s=std::to_string(k);return std::string(16-s.size(),'0')+s;}
int main(int argc,char**argv)try{
 if(argc!=7)throw std::runtime_error("DIR N WAL LIMIT THREADS PREFIX");
 size_t n=std::stoull(argv[2]),threads=std::stoull(argv[5]);std::string prefix=argv[6];
 if(!threads||n%threads||std::filesystem::exists(argv[1]))throw std::runtime_error("invalid arguments");
 DBOptions opt;opt.enableWal=std::stoi(argv[3]);opt.memTableSizeLimit=std::stoull(argv[4]);
 std::vector<std::string> keys;for(size_t i=0;i<n;++i)keys.push_back(key(i));std::string value(128,'v');
 std::vector<std::vector<size_t>> orders(threads);std::vector<std::vector<double>> lat(threads,std::vector<double>(n/threads));
 for(size_t t=0;t<threads;++t){orders[t].resize(n/threads);std::iota(orders[t].begin(),orders[t].end(),t*n/threads);std::mt19937_64 rng(20261115+t);std::shuffle(orders[t].begin(),orders[t].end(),rng);}
 lsm_diag::register_thread();auto db=std::make_unique<DB>(argv[1],opt);
 std::mutex gate;std::condition_variable cv;size_t ready=0;bool go=false;
 std::vector<std::exception_ptr> errors(threads);std::vector<Clock::time_point> ends(threads);std::vector<std::thread> workers;
 for(size_t t=0;t<threads;++t)workers.emplace_back([&,t]{lsm_diag::register_thread();{std::unique_lock<std::mutex> lock(gate);++ready;cv.notify_all();cv.wait(lock,[&]{return go;});}
 lsm_diag::Context ctx(lsm_diag::Request,lsm_diag::Put);
 try{for(size_t i=0;i<orders[t].size();++i){auto b=Clock::now();db->Put(keys[orders[t][i]],value);lat[t][i]=secs(Clock::now()-b)*1e6;}}catch(...){errors[t]=std::current_exception();}ends[t]=Clock::now();});
 Clock::time_point begin;{std::unique_lock<std::mutex> lock(gate);cv.wait(lock,[&]{return ready==threads;});begin=Clock::now();go=true;}cv.notify_all();
 for(auto&t:workers)t.join();auto end=*std::max_element(ends.begin(),ends.end());for(auto&e:errors)if(e)std::rethrow_exception(e);
 double flush_s,close_s,total_s;
 {lsm_diag::Context ctx(lsm_diag::FinalFlush,lsm_diag::Flush);auto b=Clock::now();db->Flush();auto e=Clock::now();flush_s=secs(e-b);total_s=secs(e-begin);}
 {lsm_diag::Context ctx(lsm_diag::Close,lsm_diag::Other);auto b=Clock::now();db.reset();close_s=secs(Clock::now()-b);}
 // No output, sort, RSS read, or validation between Flush return and destruction.
 std::ifstream proc("/proc/self/status");std::string line;long peak=0;while(std::getline(proc,line))if(line.rfind("VmHWM:",0)==0)peak=std::stol(line.substr(6));
 {lsm_diag::Context ctx(lsm_diag::Reopen,lsm_diag::Get);auto ro=opt;ro.enableWal=false;db=std::make_unique<DB>(argv[1],ro);for(auto&k:keys)if(db->Get(k)!=std::optional<std::string>{value})throw std::runtime_error("reopen validation mismatch");db.reset();}
 std::ofstream raw(prefix+".latency.csv");raw<<"thread,operation,latency_us\n"<<std::setprecision(12);std::vector<double> all;all.reserve(n);for(size_t t=0;t<threads;++t)for(size_t i=0;i<lat[t].size();++i){raw<<t<<','<<i<<','<<lat[t][i]<<'\n';all.push_back(lat[t][i]);}std::sort(all.begin(),all.end());
 size_t tables=0;uintmax_t bytes=0;for(auto&e:std::filesystem::directory_iterator(argv[1]))if(e.is_regular_file()){bytes+=e.file_size();tables+=e.path().extension()==".sst";}
 std::ofstream stats(prefix+".stats.json");
#ifdef LSM_DIAGNOSTICS
 lsm_diag::dump(stats);
#else
 stats<<"{\"disabled\":true}\n";
#endif
 auto p=[&](size_t pct){return all[(n*pct+99)/100-1];};
 std::cout<<std::setprecision(12)<<"{\"success\":true,\"n\":"<<n<<",\"threads\":"<<threads<<",\"request_seconds\":"<<secs(end-begin)<<",\"request_ops\":"<<n/secs(end-begin)<<",\"flush_seconds\":"<<flush_s<<",\"total_seconds\":"<<total_s<<",\"total_ops\":"<<n/total_s<<",\"close_seconds\":"<<close_s<<",\"p50_us\":"<<p(50)<<",\"p95_us\":"<<p(95)<<",\"p99_us\":"<<p(99)<<",\"max_us\":"<<all.back()<<",\"rss_kib\":"<<peak<<",\"tables\":"<<tables<<",\"disk_bytes\":"<<bytes<<"}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
