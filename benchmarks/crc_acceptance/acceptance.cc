#include "lsm/db.h"
#include "lsm/wal.h"
#include "lsm/manifest.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
static std::string key(size_t i){auto s=std::to_string(2*i);return std::string(16-s.size(),'0')+s;}
static std::string val(size_t i){auto s=std::to_string(i);return s+std::string(128-s.size(),'v');}
static void check(bool x){if(!x)throw std::runtime_error("fixture or query mismatch");}
int main(int argc,char**argv)try{
 if(argc!=4)throw std::runtime_error("MODE DIR TABLES");std::string mode=argv[1];fs::path dir=argv[2];size_t nt=std::stoull(argv[3]);const size_t n=8192;
 if(mode=="generate"){
  check(!fs::exists(dir));DBOptions opt;opt.enableWal=false;opt.memTableSizeLimit=SIZE_MAX;
  {DB db(dir.string(),opt);for(size_t t=0;t<nt;++t){for(size_t i=t;i<n;i+=nt)db.Put(key(i),val(i));db.Flush();}}
  // Separate WAL fixture, including binary data and tombstones; all CRCs from linked implementation.
  WalWriter w((dir/"legacy.wal").string());
  check(!w.Append(lsm::Entry{InternalKey{std::string("k\0\xff",3),1,ValueType::kValue},std::string("v\0\x80",3)}));
  check(!w.Append(lsm::Entry{InternalKey{"gone",2,ValueType::kDeletion},""}));check(!w.Sync());
  std::cout<<"{\"generated\":true}\n";return 0;
 }
 check(fs::exists(dir));
 {WalReader w((dir/"legacy.wal").string());lsm::Entry e;check(w.ReadNext(e)==WalReadStatus::kRecord);check(e.key.user_key==std::string("k\0\xff",3)&&e.value==std::string("v\0\x80",3)&&e.key.sequence==1);check(w.ReadNext(e)==WalReadStatus::kRecord);check(e.key.user_key=="gone"&&e.key.type==ValueType::kDeletion&&e.key.sequence==2);check(w.ReadNext(e)==WalReadStatus::kEndOfFile);}
 auto manifest=Manifest::Load((dir/"MANIFEST").string());check(manifest.Tables().size()==nt);check(manifest.FlushedSequence()==n);
 DBOptions opt;opt.enableWal=false;DB db(dir.string(),opt);
 for(size_t i=0;i<n;++i)check(db.Get(key(i))==std::optional<std::string>{val(i)});
 if(mode=="verify"){std::cout<<"{\"legacy_wal_sstable_manifest_verified\":true,\"keys\":"<<n<<"}\n";return 0;}
 check(mode=="query");std::vector<size_t> order(n);std::iota(order.begin(),order.end(),0);std::mt19937_64 rng(20261115);std::shuffle(order.begin(),order.end(),rng);
 for(auto kind:{std::string("latest"),std::string("oldest"),std::string("missing")}){
  std::vector<std::string> queries,expect;queries.reserve(n);expect.reserve(n);
  for(auto k:order){if(kind=="missing"){auto s=std::to_string(2*k+1);queries.push_back(std::string(16-s.size(),'0')+s);expect.emplace_back();}else{size_t t=kind=="latest"?nt-1:0;size_t count=(n-1-t)/nt+1;size_t i=t+(k%count)*nt;queries.push_back(key(i));expect.push_back(val(i));}}
  for(auto&q:queries)(void)db.Get(q); // identical trace warmup; OS cache, no engine block cache
  std::vector<double> lat(n);std::vector<std::optional<std::string>> results(n);
  auto b=Clock::now();for(size_t i=0;i<n;++i){auto a=Clock::now();results[i]=db.Get(queries[i]);lat[i]=std::chrono::duration<double,std::micro>(Clock::now()-a).count();}auto e=Clock::now();
  for(size_t i=0;i<n;++i)check(kind=="missing"?!results[i]:(results[i]&&*results[i]==expect[i]));
  std::sort(lat.begin(),lat.end());double sec=std::chrono::duration<double>(e-b).count();auto p=[&](size_t pct){return lat[(n*pct+99)/100-1];};
  std::cout<<std::setprecision(12)<<"{\"case\":\""<<kind<<"\",\"tables\":"<<nt<<",\"operations\":"<<n<<",\"seconds\":"<<sec<<",\"ops_per_second\":"<<n/sec<<",\"p50_us\":"<<p(50)<<",\"p95_us\":"<<p(95)<<",\"p99_us\":"<<p(99)<<",\"max_us\":"<<lat.back()<<"}\n";
 }
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
